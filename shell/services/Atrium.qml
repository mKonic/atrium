pragma Singleton

import QtQuick
import Quickshell
import Quickshell.Io

// The compositor, over its control socket (JSON lines). One connection
// listens for events; another sends requests and matches replies by id.
// Any event just re-reads windows, spaces and outputs: they are small.
Singleton {
    id: root

    property var windows: []
    property var spaces: []
    property var outputs: []
    property var settings: ({})  // the settings store: key → value
    readonly property var focusedWindow: windows.find(w => w.focused) ?? null
    readonly property var shownSecret: spaces.find(s => s.secret && s.shown) ?? null

    readonly property string socketPath: `${Quickshell.env("XDG_RUNTIME_DIR")}/atrium.${Quickshell.env("WAYLAND_DISPLAY")}.sock`

    property int _nextId: 1
    property var _pending: ({})

    // The numbered space shown on an output.
    function activeSpace(output: string): int {
        return spaces.find(s => !s.secret && s.shown && s.output === output)?.number ?? 1;
    }

    function spacesOn(output: string): var {
        return spaces.filter(s => !s.secret && s.output === output);
    }

    function windowsOn(output: string, space: int): var {
        return windows.filter(w => !w.secret && w.output === output && w.space === String(space));
    }

    function request(req: var, onResult: var): void {
        const id = _nextId++;
        req.id = id;
        if (onResult)
            _pending[id] = onResult;
        requests.write(JSON.stringify(req) + "\n");
        requests.flush();
    }

    function refresh(): void {
        request({ cmd: "windows" }, r => root.windows = r);
        request({ cmd: "spaces" }, r => root.spaces = r);
        request({ cmd: "outputs" }, r => root.outputs = r);
        request({ cmd: "settings.get" }, r => root.settings = r);
    }

    function setting(key: string, fallback: var): var {
        return settings[key] ?? fallback;
    }

    function setSetting(key: string, value: var): void {
        request({ cmd: "settings.set", key: key, value: value });
    }

    function closeWindow(id: int): void {
        request({ cmd: "window.close", window: id });
    }

    function switchSpace(n: int): void {
        request({ cmd: "space.switch", number: n });
    }

    function toggleSecret(name: string): void {
        request({ cmd: "secret.toggle", name: name });
    }

    function focusWindow(id: int): void {
        request({ cmd: "window.focus", window: id });
    }

    function action(name: string, arg: var): void {
        const req = { cmd: "action", name: name };
        if (arg !== undefined)
            req.arg = String(arg);
        request(req);
    }

    Socket {
        id: requests

        path: root.socketPath
        connected: true

        onConnectedChanged: {
            if (connected)
                root.refresh();
        }

        parser: SplitParser {
            onRead: line => {
                let reply;
                try {
                    reply = JSON.parse(line);
                } catch (e) {
                    return;
                }
                const done = root._pending[reply.id];
                if (done === undefined)
                    return;
                delete root._pending[reply.id];
                if (reply.ok)
                    done(reply.result);
            }
        }
    }

    Socket {
        id: events

        path: root.socketPath
        connected: true

        onConnectedChanged: {
            if (!connected)
                return;
            write(JSON.stringify({ cmd: "subscribe", topics: ["windows", "spaces", "outputs", "settings"] }) + "\n");
            flush();
        }

        parser: SplitParser {
            onRead: coalesce.restart()
        }
    }

    // A burst of events (a space switch sends several) costs one refresh.
    Timer {
        id: coalesce

        interval: 10
        onTriggered: root.refresh()
    }

    // The compositor restarted or the shell came up first: keep trying.
    Timer {
        interval: 1000
        repeat: true
        running: !requests.connected || !events.connected
        onTriggered: {
            requests.connected = true;
            events.connected = true;
        }
    }
}
