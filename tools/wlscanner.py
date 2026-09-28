#!/usr/bin/env python3
"""C++ server bindings from a Wayland protocol XML.

    wlscanner.py PROTOCOL.xml OUT.hpp OUT.cpp

One class per interface, deriving from atrium::wl::Resource (src/wl/resource.hpp,
which has the lifetime model): typed request handlers set with on_<request>(),
events sent with send_<event>() (skipped for clients bound below the event's
version), enums as enum classes. The interface tables are wayland-scanner's
private-code, built alongside.

Argument types: int -> int32_t, uint -> uint32_t, fixed -> double, string ->
const char*, array -> wl_array*, fd -> int (a request handler owns it), object
-> the generated class when the interface is in the same file, else
wl_resource*; a request's new_id is the uint32_t id to construct with
wl::make<T>(), an event's is the already-made object.
"""
import re
import sys
import xml.etree.ElementTree as ET

CPP_KEYWORDS = {
    'alignas', 'alignof', 'and', 'asm', 'auto', 'bool', 'break', 'case', 'catch', 'char',
    'class', 'const', 'constexpr', 'continue', 'default', 'delete', 'do', 'double', 'else',
    'enum', 'explicit', 'export', 'extern', 'false', 'float', 'for', 'friend', 'goto', 'if',
    'inline', 'int', 'long', 'mutable', 'namespace', 'new', 'noexcept', 'not', 'nullptr',
    'operator', 'or', 'private', 'protected', 'public', 'register', 'return', 'short',
    'signed', 'sizeof', 'static', 'struct', 'switch', 'template', 'this', 'throw', 'true',
    'try', 'typedef', 'typename', 'union', 'unsigned', 'using', 'virtual', 'void',
    'volatile', 'while', 'xor', 'interface', 'resource', 'version', 'client',
}


def camel(name):
    return ''.join(p[:1].upper() + p[1:] for p in name.split('_') if p)


def ident(name):
    return name + '_' if name in CPP_KEYWORDS else name


def enum_entry(name):
    n = camel(name)
    return '_' + n if n[:1].isdigit() else n


class Arg:
    def __init__(self, el):
        self.name = ident(el.get('name'))
        self.type = el.get('type')
        self.interface = el.get('interface')
        self.nullable = el.get('allow-null') == 'true'


class Message:
    def __init__(self, el, opcode):
        self.name = el.get('name')
        self.opcode = opcode
        self.since = int(el.get('since', '1'))
        self.destructor = el.get('type') == 'destructor'
        self.args = [Arg(a) for a in el.findall('arg')]
        d = el.find('description')
        self.summary = d.get('summary') if d is not None else None


class Enum:
    def __init__(self, el):
        self.name = el.get('name')
        self.bitfield = el.get('bitfield') == 'true'
        self.entries = [(e.get('name'), e.get('value')) for e in el.findall('entry')]


class Interface:
    def __init__(self, el):
        self.name = el.get('name')
        self.cls = camel(self.name)
        self.version = int(el.get('version'))
        self.requests = [Message(m, i) for i, m in enumerate(el.findall('request'))]
        self.events = [Message(m, i) for i, m in enumerate(el.findall('event'))]
        self.enums = [Enum(e) for e in el.findall('enum')]


def request_params(iface, msg, own):
    """C++ parameter list and the expressions unpacking wl_argument `a`."""
    params, unpack = [], []
    i = 0
    for arg in msg.args:
        t = arg.type
        if t == 'new_id' and not arg.interface:
            params += ['const char* interface', 'uint32_t version', f'uint32_t {arg.name}']
            unpack += [f'a[{i}].s', f'a[{i + 1}].u', f'a[{i + 2}].n']
            i += 3
            continue
        if t == 'int':
            params.append(f'int32_t {arg.name}')
            unpack.append(f'a[{i}].i')
        elif t == 'uint':
            params.append(f'uint32_t {arg.name}')
            unpack.append(f'a[{i}].u')
        elif t == 'fixed':
            params.append(f'double {arg.name}')
            unpack.append(f'wl_fixed_to_double(a[{i}].f)')
        elif t == 'string':
            params.append(f'const char* {arg.name}')
            unpack.append(f'a[{i}].s')
        elif t == 'array':
            params.append(f'wl_array* {arg.name}')
            unpack.append(f'a[{i}].a')
        elif t == 'fd':
            params.append(f'int {arg.name}')
            unpack.append(f'a[{i}].h')
        elif t == 'new_id':
            params.append(f'uint32_t {arg.name}')
            unpack.append(f'a[{i}].n')
        elif t == 'object':
            res = f'reinterpret_cast<wl_resource*>(a[{i}].o)'
            if arg.interface in own:
                params.append(f'{own[arg.interface]}* {arg.name}')
                unpack.append(f'{own[arg.interface]}::from({res})')
            else:
                params.append(f'wl_resource* {arg.name}')
                unpack.append(res)
        else:
            raise SystemExit(f'{iface.name}.{msg.name}: unknown type {t}')
        i += 1
    return params, unpack


def event_params(msg, own):
    params, pass_ = [], []
    for arg in msg.args:
        t = arg.type
        if t == 'int':
            params.append(f'int32_t {arg.name}')
            pass_.append(arg.name)
        elif t == 'uint':
            params.append(f'uint32_t {arg.name}')
            pass_.append(arg.name)
        elif t == 'fixed':
            params.append(f'double {arg.name}')
            pass_.append(f'wl_fixed_from_double({arg.name})')
        elif t == 'string':
            params.append(f'const char* {arg.name}')
            pass_.append(arg.name)
        elif t == 'array':
            params.append(f'wl_array* {arg.name}')
            pass_.append(arg.name)
        elif t == 'fd':
            params.append(f'int {arg.name}')
            pass_.append(arg.name)
        elif t in ('object', 'new_id'):
            if arg.interface in own:
                params.append(f'{own[arg.interface]}* {arg.name}')
                pass_.append(f'({arg.name} ? {arg.name}->resource() : nullptr)')
            else:
                params.append(f'wl_resource* {arg.name}')
                pass_.append(arg.name)
        else:
            raise SystemExit(f'event {msg.name}: unknown type {t}')
    return params, pass_


def header(protocol, ifaces, own):
    out = ['// Generated by tools/wlscanner.py from ' + protocol + '.xml. Do not edit.',
           '#pragma once', '#include "wl/resource.hpp"', '',
           '#include <cstdint>', '#include <functional>', '']
    out.append('extern "C" {')
    for it in ifaces:
        out.append(f'extern const struct wl_interface {it.name}_interface;')
    out += ['}', '', 'namespace atrium::wl {', '']
    for it in ifaces:
        out.append(f'class {it.cls};')
    out.append('')
    for it in ifaces:
        out.append(f'class {it.cls} : public Resource {{')
        out.append('public:')
        out.append(f'    static constexpr uint32_t kVersion = {it.version};')
        out.append(f'    static const wl_interface* interface() {{ return &{it.name}_interface; }}')
        for en in it.enums:
            out.append(f'    enum class {camel(en.name)} : uint32_t {{')
            for name, value in en.entries:
                out.append(f'        {enum_entry(name)} = {value},')
            out.append('    };')
        out.append('')
        out.append(f'    {it.cls}(wl_client* client, uint32_t version, uint32_t id);')
        out.append(f'    // The object behind `r`, or null if it is not a live {it.name}.')
        out.append(f'    static {it.cls}* from(wl_resource* r) {{')
        out.append(f'        return static_cast<{it.cls}*>(lookup(r, interface(), &kTag));')
        out.append('    }')
        out.append('')
        for m in it.requests:
            params, _ = request_params(it, m, own)
            sig = ', '.join([f'{it.cls}* self'] + params)
            if m.summary:
                out.append(f'    // {m.summary}' + (' (a destructor: the object goes after the handler)'
                                                     if m.destructor else ''))
            out.append(f'    void on_{m.name}(std::function<void({sig})> fn) {{ {m.name}_ = std::move(fn); }}')
        for m in it.events:
            params, _ = event_params(m, own)
            if m.summary:
                out.append(f'    // {m.summary}' + (f' (since version {m.since})' if m.since > 1 else ''))
            out.append(f'    void send_{m.name}({", ".join(params)});')
        out.append('')
        out.append('protected:')
        out.append('    void clear_handlers() override;')
        out.append('')
        out.append('private:')
        out.append('    static constexpr char kTag = 0;')
        out.append('    static int dispatch(const void*, void* target, uint32_t opcode, const wl_message*, wl_argument* a);')
        for m in it.requests:
            params, _ = request_params(it, m, own)
            sig = ', '.join([f'{it.cls}*'] + [p.rsplit(' ', 1)[0] for p in params])
            out.append(f'    std::function<void({sig})> {m.name}_;')
        out.append('};')
        out.append('')
    out.append('} // namespace atrium::wl')
    return '\n'.join(out) + '\n'


def source(protocol, hpp, ifaces, own):
    out = ['// Generated by tools/wlscanner.py from ' + protocol + '.xml. Do not edit.',
           f'#include "{hpp}"', '', '#include <unistd.h>', '', 'namespace atrium::wl {', '']
    for it in ifaces:
        c = it.cls
        out.append(f'{c}::{c}(wl_client* client, uint32_t version, uint32_t id)')
        out.append(f'    : Resource(client, interface(), version, id, &dispatch, &kTag) {{}}')
        out.append('')
        out.append(f'void {c}::clear_handlers() {{')
        for m in it.requests:
            out.append(f'    {m.name}_ = nullptr;')
        out.append('}')
        out.append('')
        out.append(f'int {c}::dispatch(const void*, void* target, uint32_t opcode, const wl_message*, wl_argument* a) {{')
        out.append(f'    auto* self = static_cast<{c}*>(of_target(target));')
        out.append('    if (!self)')
        out.append('        return 0;')
        if not it.requests:
            out.append('    (void)opcode;')
            out.append('    (void)a;')
        else:
            out.append('    switch (opcode) {')
            for m in it.requests:
                _, unpack = request_params(it, m, own)
                call = ', '.join(['self'] + unpack)
                fds = [i for i, arg in enumerate(m.args) if arg.type == 'fd']
                out.append(f'    case {m.opcode}:')
                if m.destructor:
                    out.append('        {')
                    out.append('            std::weak_ptr<void> alive = self->token();')
                    out.append(f'            if (self->{m.name}_ && !self->inert())')
                    out.append(f'                self->{m.name}_({call});')
                    out.append('            if (!alive.expired())')
                    out.append('                self->destroy();')
                    out.append('        }')
                else:
                    out.append(f'        if (self->{m.name}_ && !self->inert())')
                    out.append(f'            self->{m.name}_({call});')
                    if fds:
                        # Nobody took the descriptors: don't leak them.
                        out.append('        else')
                        out.append('            ' + ' '.join(f'close(a[{i}].h);' for i in fds))
                out.append('        break;')
            out.append('    }')
        out.append('    return 0;')
        out.append('}')
        out.append('')
        for m in it.events:
            params, pass_ = event_params(m, own)
            out.append(f'void {c}::send_{m.name}({", ".join(params)}) {{')
            cond = '!resource()' + (f' || version() < {m.since}' if m.since > 1 else '')
            out.append(f'    if ({cond})')
            out.append('        return;')
            out.append(f'    wl_resource_post_event(resource(), {m.opcode}' + ''.join(', ' + p for p in pass_) + ');')
            out.append('}')
            out.append('')
    out.append('} // namespace atrium::wl')
    return '\n'.join(out) + '\n'


def main():
    if len(sys.argv) != 4:
        raise SystemExit(__doc__)
    xml, hpp, cpp = sys.argv[1:]
    root = ET.parse(xml).getroot()
    protocol = root.get('name')
    ifaces = [Interface(i) for i in root.findall('interface')]
    own = {it.name: it.cls for it in ifaces}
    hpp_name = re.sub(r'.*/', '', hpp)
    with open(hpp, 'w') as f:
        f.write(header(protocol, ifaces, own))
    with open(cpp, 'w') as f:
        f.write(source(protocol, hpp_name, ifaces, own))


if __name__ == '__main__':
    main()
