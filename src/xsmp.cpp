#include "xsmp.hpp"

#include "server.hpp"
#include "wlr.hpp"

#include <X11/ICE/ICElib.h>
#include <X11/ICE/ICEutil.h>
#include <X11/SM/SMlib.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <random>

#include <sys/stat.h>

// libICE's own (Xtrans): which transports it listens on. Not over the
// network, as ksmserver has it.
extern "C" int _IceTransNoListen(const char* protocol);

namespace atrium {

namespace {

// A client given this long to save, unless it is asking the user something.
constexpr int kSaveTimeoutMs = 10000;

char* xstrdup(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

} // namespace

struct Xsmp::Client {
    SmsConn conn = nullptr;
    std::string id, program;
    bool saving = false;  // asked to save, not done yet
};

struct Xsmp::Listener {
    Xsmp* xsmp;
    IceListenObj obj;
    wl_event_source* source = nullptr;
};

// libSM calls these, with the Client as manager data.
struct XsmpCallbacks {
    static Xsmp* self;

    static Xsmp::Client* client(SmPointer data) {
        return static_cast<Xsmp::Client*>(data);
    }

    static Status register_client(SmsConn conn, SmPointer data, char* previous) {
        Xsmp::Client* c = client(data);
        // A previous id is only the client's again if it isn't taken now.
        bool taken = false;
        if (previous)
            for (const auto& o : self->clients_)
                taken |= o.get() != c && o->id == previous;
        char* id = previous && *previous && !taken ? xstrdup(previous) : SmsGenerateClientID(conn);
        if (!id)
            return 0;
        c->id = id;
        SmsRegisterClientReply(conn, id);
        std::free(id);
        if (previous)
            std::free(previous);
        // A new client is asked for its state once, as the protocol has it.
        else
            SmsSaveYourself(conn, SmSaveLocal, False, SmInteractStyleNone, False);
        return 1;
    }

    static void interact_request(SmsConn, SmPointer data, int) {
        Xsmp::Client* c = client(data);
        if (!self->saving_) {
            SmsInteract(c->conn);  // not ours to refuse outside a logout
            return;
        }
        self->waiting_.push_back(c);
        self->next_interaction();
    }

    static void interact_done(SmsConn, SmPointer data, Bool cancel) {
        Xsmp::Client* c = client(data);
        if (self->interacting_ == c)
            self->interacting_ = nullptr;
        if (cancel && self->saving_) {
            self->finish_saving(c->program.empty() ? "An app" : c->program);
            return;
        }
        self->next_interaction();
    }

    static void save_request(SmsConn conn, SmPointer, int, Bool, int, Bool, Bool) {
        // The app asking to save itself (or everyone): nothing to coordinate.
        SmsSaveComplete(conn);
    }

    static void phase2_request(SmsConn conn, SmPointer) {
        SmsSaveYourselfPhase2(conn);
    }

    static void save_done(SmsConn, SmPointer data, Bool) {
        client(data)->saving = false;
        self->check_saved();
    }

    static void close_connection(SmsConn conn, SmPointer data, int count, char** reasons) {
        Xsmp::Client* c = client(data);
        SmFreeReasons(count, reasons);
        IceConn ice = SmsGetIceConnection(conn);
        SmsCleanUp(conn);
        IceSetShutdownNegotiation(ice, False);
        IceCloseConnection(ice);
        std::erase(self->waiting_, c);
        if (self->interacting_ == c)
            self->interacting_ = nullptr;
        std::erase_if(self->clients_, [c](const auto& x) { return x.get() == c; });
        if (self->saving_) {
            self->next_interaction();
            self->check_saved();
        }
    }

    static void set_properties(SmsConn, SmPointer data, int n, SmProp** props) {
        Xsmp::Client* c = client(data);
        for (int i = 0; i < n; ++i) {
            if (std::strcmp(props[i]->name, SmProgram) == 0 && props[i]->num_vals > 0) {
                std::string p(static_cast<const char*>(props[i]->vals[0].value), size_t(props[i]->vals[0].length));
                c->program = p.substr(p.rfind('/') + 1);
            }
            SmFreeProperty(props[i]);
        }
        std::free(props);
    }

    static void delete_properties(SmsConn, SmPointer, int n, char** names) {
        for (int i = 0; i < n; ++i)
            std::free(names[i]);
        std::free(names);
    }

    static void get_properties(SmsConn conn, SmPointer) {
        SmsReturnProperties(conn, 0, nullptr);
    }

    static Status new_client(SmsConn conn, SmPointer, unsigned long* mask, SmsCallbacks* cb, char**) {
        auto owned = std::make_unique<Xsmp::Client>();
        Xsmp::Client* c = owned.get();
        c->conn = conn;
        self->clients_.push_back(std::move(owned));
        *mask = 0;
        auto set = [&](auto& slot, auto fn) {
            slot.callback = fn;
            slot.manager_data = c;
        };
        set(cb->register_client, register_client);
        set(cb->interact_request, interact_request);
        set(cb->interact_done, interact_done);
        set(cb->save_yourself_request, save_request);
        set(cb->save_yourself_phase2_request, phase2_request);
        set(cb->save_yourself_done, save_done);
        set(cb->close_connection, close_connection);
        set(cb->set_properties, set_properties);
        set(cb->delete_properties, delete_properties);
        set(cb->get_properties, get_properties);
        return 1;
    }

    // Only the cookie lets a client in.
    static Bool no_host_auth(char*) {
        return False;
    }

    static void watch(IceConn conn, IcePointer, Bool opening, IcePointer*) {
        self->watch(conn, opening);
    }

    // libICE's own handlers exit the process: a client's broken connection
    // must never take atrium down.
    static void io_error(IceConn) {}
    static void ice_error(IceConn, Bool, int code, unsigned long, int, int, IcePointer) {
        wlr_log(WLR_DEBUG, "xsmp: ICE error %d", code);
    }

    // A connection that broke without saying goodbye: its client goes.
    static void drop(IceConn ice) {
        for (auto& c : self->clients_)
            if (SmsGetIceConnection(c->conn) == ice) {
                Xsmp::Client* gone = c.get();
                SmsCleanUp(gone->conn);
                std::erase(self->waiting_, gone);
                if (self->interacting_ == gone)
                    self->interacting_ = nullptr;
                std::erase_if(self->clients_, [gone](const auto& x) { return x.get() == gone; });
                break;
            }
        IceSetShutdownNegotiation(ice, False);
        IceCloseConnection(ice);
        if (self->saving_) {
            self->next_interaction();
            self->check_saved();
        }
    }
};

Xsmp* XsmpCallbacks::self = nullptr;

Xsmp::Xsmp(Server& server) : server_(server) {
    if (XsmpCallbacks::self)
        return;  // one per process: libSM's state is global
    XsmpCallbacks::self = this;
    IceSetIOErrorHandler(XsmpCallbacks::io_error);
    IceSetErrorHandler(XsmpCallbacks::ice_error);
    char error[256] = "";
    if (!SmsInitialize("atrium", "1.0", XsmpCallbacks::new_client, nullptr, XsmpCallbacks::no_host_auth,
                       sizeof error, error)) {
        wlr_log(WLR_ERROR, "xsmp: %s", error);
        return;
    }
    IceAddConnectionWatch(XsmpCallbacks::watch, nullptr);
    _IceTransNoListen("tcp");
    int count = 0;
    IceListenObj* objs = nullptr;
    if (!IceListenForConnections(&count, &objs, sizeof error, error) || count == 0) {
        wlr_log(WLR_ERROR, "xsmp: can't listen: %s", error);
        return;
    }
    // A cookie for each address, for ICE and XSMP alike, in ~/.ICEauthority
    // (where the apps' libICE looks) and in our own libICE's memory.
    std::random_device rd;
    std::vector<IceAuthDataEntry> entries;
    const char* file = IceAuthFileName();
    const bool locked = file && IceLockAuthFile(file, 10, 2, 600) == IceAuthLockSuccess;
    FILE* out = locked ? std::fopen(file, "ab") : nullptr;
    for (int i = 0; i < count; ++i) {
        IceSetHostBasedAuthProc(objs[i], XsmpCallbacks::no_host_auth);
        char* id = IceGetListenConnectionString(objs[i]);
        unsigned char cookie[16];
        for (auto& b : cookie)
            b = static_cast<unsigned char>(rd());
        for (const char* proto : {"ICE", "XSMP"}) {
            IceAuthDataEntry e{};
            e.protocol_name = xstrdup(proto);
            e.network_id = xstrdup(id);
            e.auth_name = xstrdup("MIT-MAGIC-COOKIE-1");
            e.auth_data_length = sizeof cookie;
            e.auth_data = static_cast<char*>(std::malloc(sizeof cookie));
            std::memcpy(e.auth_data, cookie, sizeof cookie);
            entries.push_back(e);
            if (out) {
                IceAuthFileEntry f{};
                f.protocol_name = e.protocol_name;
                f.network_id = e.network_id;
                f.auth_name = e.auth_name;
                f.auth_data_length = e.auth_data_length;
                f.auth_data = e.auth_data;
                f.protocol_data_length = 0;
                f.protocol_data = const_cast<char*>("");
                IceWriteAuthFileEntry(out, &f);
            }
        }
        auth_ids_.push_back(id);
        std::free(id);

        auto l = std::make_unique<Listener>(Listener{this, objs[i], nullptr});
        Listener* raw = l.get();
        l->source = wl_event_loop_add_fd(server_.loop, IceGetListenConnectionNumber(objs[i]), WL_EVENT_READABLE,
                                         [](int, uint32_t, void* d) {
                                             auto* listener = static_cast<Listener*>(d);
                                             listener->xsmp->accept(*listener);
                                             return 0;
                                         },
                                         raw);
        listeners_.push_back(std::move(l));
    }
    if (out) {
        std::fclose(out);
        chmod(file, 0600);  // cookies: ours alone
    }
    if (locked)
        IceUnlockAuthFile(file);
    IceSetPaAuthData(int(entries.size()), entries.data());
    // libICE copied them.
    for (auto& e : entries) {
        std::free(e.protocol_name);
        std::free(e.network_id);
        std::free(e.auth_name);
        std::free(e.auth_data);
    }
    char* ids = IceComposeNetworkIdList(count, objs);
    address_ = ids ? ids : "";
    std::free(ids);
    wlr_log(WLR_INFO, "xsmp: SESSION_MANAGER=%s", address_.c_str());
}

Xsmp::~Xsmp() {
    if (XsmpCallbacks::self != this)
        return;
    if (timeout_)
        wl_event_source_remove(timeout_);
    for (auto& c : clients_) {
        IceConn ice = SmsGetIceConnection(c->conn);
        SmsCleanUp(c->conn);
        IceSetShutdownNegotiation(ice, False);
        IceCloseConnection(ice);
    }
    clients_.clear();
    for (auto& [conn, src] : connections_)
        wl_event_source_remove(src);
    connections_.clear();
    for (auto& l : listeners_)
        wl_event_source_remove(l->source);
    IceRemoveConnectionWatch(XsmpCallbacks::watch, nullptr);
    // Our cookies leave ~/.ICEauthority with us.
    const char* file = IceAuthFileName();
    if (file && !auth_ids_.empty() && IceLockAuthFile(file, 10, 2, 600) == IceAuthLockSuccess) {
        std::vector<IceAuthFileEntry*> keep;
        if (FILE* in = std::fopen(file, "rb")) {
            while (IceAuthFileEntry* e = IceReadAuthFileEntry(in)) {
                if (std::ranges::find(auth_ids_, std::string(e->network_id)) != auth_ids_.end())
                    IceFreeAuthFileEntry(e);
                else
                    keep.push_back(e);
            }
            std::fclose(in);
        }
        if (FILE* out = std::fopen(file, "wb")) {
            for (IceAuthFileEntry* e : keep)
                IceWriteAuthFileEntry(out, e);
            std::fclose(out);
        }
        for (IceAuthFileEntry* e : keep)
            IceFreeAuthFileEntry(e);
        IceUnlockAuthFile(file);
    }
    XsmpCallbacks::self = nullptr;
}

size_t Xsmp::clients() const {
    return size_t(std::ranges::count_if(clients_, [](const auto& c) { return !c->id.empty(); }));
}

void Xsmp::accept(Listener& l) {
    IceAcceptStatus status;
    IceConn conn = IceAcceptConnection(l.obj, &status);
    if (!conn || status != IceAcceptSuccess)
        return;
    // Its setup goes on in IceProcessMessages, as its fd says.
    IceSetShutdownNegotiation(conn, False);
}

void Xsmp::watch(IceConn conn, bool opening) {
    if (opening) {
        wl_event_source* src = wl_event_loop_add_fd(server_.loop, IceConnectionNumber(conn), WL_EVENT_READABLE,
                                                    [](int, uint32_t, void* d) {
                                                        auto ice = static_cast<IceConn>(d);
                                                        if (IceProcessMessages(ice, nullptr, nullptr) ==
                                                            IceProcessMessagesIOError)
                                                            XsmpCallbacks::drop(ice);
                                                        return 0;
                                                    },
                                                    conn);
        connections_.emplace_back(conn, src);
        return;
    }
    std::erase_if(connections_, [conn](auto& p) {
        if (p.first != conn)
            return false;
        wl_event_source_remove(p.second);
        return true;
    });
}

void Xsmp::save_all(std::function<void(const std::string&)> done) {
    done_ = std::move(done);
    saving_ = true;
    shutting_down_ = true;
    interacting_ = nullptr;
    waiting_.clear();
    for (auto& c : clients_) {
        if (c->id.empty())
            continue;
        c->saving = true;
        SmsSaveYourself(c->conn, SmSaveBoth, True, SmInteractStyleAny, False);
    }
    timeout_ = wl_event_loop_add_timer(server_.loop, [](void* d) {
        auto* self = static_cast<Xsmp*>(d);
        // Still asking the user: theirs to answer, however long.
        if (self->interacting_ || !self->waiting_.empty()) {
            wl_event_source_timer_update(self->timeout_, kSaveTimeoutMs);
            return 0;
        }
        for (auto& c : self->clients_)
            if (c->saving)
                wlr_log(WLR_INFO, "xsmp: %s didn't say it saved; going on", c->program.c_str());
        self->finish_saving("");
        return 0;
    }, this);
    wl_event_source_timer_update(timeout_, kSaveTimeoutMs);
    check_saved();
}

void Xsmp::next_interaction() {
    if (!saving_ || interacting_ || waiting_.empty())
        return;
    interacting_ = waiting_.front();
    waiting_.erase(waiting_.begin());
    SmsInteract(interacting_->conn);
}

void Xsmp::check_saved() {
    if (!saving_)
        return;
    for (const auto& c : clients_)
        if (c->saving)
            return;
    finish_saving("");
}

void Xsmp::finish_saving(const std::string& holdout) {
    if (!saving_)
        return;
    saving_ = false;
    if (timeout_) {
        wl_event_source_remove(timeout_);
        timeout_ = nullptr;
    }
    if (!holdout.empty())
        cancel();
    auto done = std::move(done_);
    done_ = nullptr;
    if (done)
        done(holdout);
}

void Xsmp::cancel() {
    if (!shutting_down_)
        return;
    shutting_down_ = false;
    for (auto& c : clients_)
        if (!c->id.empty()) {
            c->saving = false;
            SmsShutdownCancelled(c->conn);
        }
    waiting_.clear();
    interacting_ = nullptr;
}

void Xsmp::die() {
    shutting_down_ = false;
    for (auto& c : clients_)
        if (!c->id.empty())
            SmsDie(c->conn);
}

} // namespace atrium
