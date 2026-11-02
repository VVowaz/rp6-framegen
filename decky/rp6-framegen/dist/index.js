// RP6 FrameGen: Decky panel for the rp6fg frame generation layer.
// Hand-written against Decky's globals (SP_REACT, DFL) and loader API v2, no build step.
const manifest = { name: "RP6 FrameGen" };
const internalAPIConnection = window.__DECKY_SECRET_INTERNALS_DO_NOT_USE_OR_YOU_WILL_BE_FIRED_deckyLoaderAPIInit;
if (!internalAPIConnection) {
    throw new Error("[RP6 FrameGen] Decky loader API is not initialized");
}
let api;
try {
    api = internalAPIConnection.connect(2, manifest.name);
}
catch {
    api = internalAPIConnection.connect(1, manifest.name);
}
const call = (...args) => api.call(...args);

const h = SP_REACT.createElement;
const { useState, useEffect, useRef } = SP_REACT;
const { PanelSection, PanelSectionRow, ToggleField, SliderField, DropdownItem, Field } = DFL;

// ---- per-game launch option ----------------------------------------------------
// The layer is enabled by the RP6FG=1 environment assignment in front of
// %command%. Only that prefix is ever touched: everything from %command% on
// (wrappers, game arguments, quoted strings) is kept byte for byte. Armada's
// own plugin replaces %command% with "armada-game-launch %command%", which
// keeps our prefix in front.
const COMMAND = "%command%";
// Only RP6FG= itself: other RP6FG_* variables (logging, dumps) belong to the user.
const FG_ENV = /(^|\s)RP6FG=\S*(?=\s|$)/g;
// [env prefix, rest starting at %command%]; options without %command% are game arguments.
const splitOpts = (opts) => {
    const o = opts || "";
    const i = o.indexOf(COMMAND);
    return i < 0 ? ["", o.trim() ? `${COMMAND} ${o.trim()}` : ""] : [o.slice(0, i), o.slice(i)];
};
// On when the last RP6FG= assignment before %command% is 1 (the shell keeps the last one).
const hasFG = (opts) => {
    const values = [...splitOpts(opts)[0].matchAll(/(?:^|\s)RP6FG=(\S*)/g)].map((m) => m[1]);
    return values.length > 0 && values[values.length - 1] === "1";
};
const stripPrefix = (prefix) => prefix.replace(FG_ENV, "").replace(/^\s+/, "").replace(/\s{2,}$/, " ");
const addFG = (opts) => {
    const [prefix, rest] = splitOpts(opts);
    const kept = stripPrefix(prefix);
    return `RP6FG=1 ${kept}${rest || COMMAND}`;
};
const removeFG = (opts) => {
    const [prefix, rest] = splitOpts(opts);
    const o = stripPrefix(prefix) + rest;
    return o.trim() === COMMAND ? "" : o;
};

// Fresh app details (appDetailsStore can hold a stale copy of the launch options).
const fetchLaunchOptions = (appid) => new Promise((resolve) => {
    let done = false;
    let handle = null;
    const finish = (details) => {
        if (done) return;
        done = true;
        setTimeout(() => { try { handle && handle.unregister(); } catch { } }, 0);
        resolve(details ? details.strLaunchOptions || "" : null);
    };
    try {
        handle = SteamClient.Apps.RegisterForAppDetails(Number(appid), finish);
    }
    catch {
        finish(null);
    }
    setTimeout(() => finish(null), 3000);
});

const installedGames = () => {
    try {
        return (window.appStore.allApps || [])
            .filter((a) => a.app_type === 1 && a.per_client_data && a.per_client_data[0] && a.per_client_data[0].installed)
            .sort((a, b) => (b.rt_last_time_played || 0) - (a.rt_last_time_played || 0))
            .map((a) => ({ appid: a.appid, name: a.display_name }));
    }
    catch {
        return [];
    }
};
const runningApp = () => {
    try {
        const r = DFL.Router && DFL.Router.MainRunningApp;
        return r ? { appid: Number(r.appid), name: r.display_name } : null;
    }
    catch {
        return null;
    }
};

const fps = (hz, m) => Math.round(hz / m);

function Content() {
    const [state, setState] = useState(null);
    const [games] = useState(installedGames);
    const running = runningApp();
    const [pick, setPick] = useState(running ? running.appid : (games[0] ? games[0].appid : null));
    const [opts, setOpts] = useState(null);
    const [note, setNote] = useState("");
    const alive = useRef(true);

    const pickRef = useRef(pick);
    pickRef.current = pick;
    useEffect(() => {
        alive.current = true;
        const poll = () => call("get_state", pickRef.current || 0)
            .then((s) => { if (alive.current && s.profile_appid === (pickRef.current || 0)) setState(s); }).catch(() => { });
        poll();
        const t = setInterval(poll, 1000);
        return () => { alive.current = false; clearInterval(t); };
    }, []);
    useEffect(() => {
        call("get_state", pick || 0).then((s) => { if (alive.current && s.profile_appid === (pickRef.current || 0)) setState(s); }).catch(() => { });
    }, [pick]);
    const setProfile = (key, value) => {
        const appid = pick || 0;
        call("set_profile", appid, key, value).then((s) => { if (alive.current && pickRef.current === appid) setState(s); }).catch(() => { });
    };
    useEffect(() => {
        // A slow answer for a previously picked game must not land on this one.
        let cancelled = false;
        setOpts(null);
        setNote("");
        if (pick) fetchLaunchOptions(pick).then((o) => { if (!cancelled && alive.current) setOpts(o); });
        return () => { cancelled = true; };
    }, [pick]);

    const busy = useRef(false);
    const toggleGame = async (on) => {
        if (opts === null || !pick || busy.current) return;
        busy.current = true;
        const appid = pick;
        try {
            // Re-read right before writing: Armada's plugin may have rewritten them meanwhile.
            const current = await fetchLaunchOptions(appid);
            if (current === null) { setNote("Could not read the launch options"); return; }
            const next = on ? addFG(current) : removeFG(current);
            SteamClient.Apps.SetAppLaunchOptions(Number(appid), next);
            if (!alive.current || pickRef.current !== appid) return;  // the user moved on to another game
            setOpts(next);
            const isRunning = running && running.appid === appid;
            setNote(isRunning ? "Restart the game to apply" : "");
        }
        finally {
            busy.current = false;
        }
    };

    // Each multiplier change makes the game re-create its swapchain: commit only
    // once the slider has rested for a moment.
    const multTimer = useRef(null);
    const multPending = useRef(null);
    const [multDraft, setMultDraft] = useState(null);
    const commitMult = () => {
        const v = multPending.current;
        multPending.current = null;
        if (v === null) return;
        const [appid, mult] = v;
        call("set_profile", appid, "mult", mult).then((st) => {
            if (!alive.current) return;
            if (pickRef.current === appid) setState(st);
            if (multPending.current === null) setMultDraft(null);  // keep a newer draft
        }).catch(() => { if (alive.current && multPending.current === null) setMultDraft(null); });
    };
    const setMult = (v) => {
        setMultDraft([pick || 0, v]);
        multPending.current = [pick || 0, v];
        clearTimeout(multTimer.current);
        multTimer.current = setTimeout(commitMult, 600);
    };
    // Closing the panel must not drop a pending change.
    useEffect(() => () => { clearTimeout(multTimer.current); commitMult(); }, []);

    const live = state && state.live;
    const prof = state && state.profile_appid === (pick || 0) ? state.profile : null;
    const modes = (state && state.refresh_modes && state.refresh_modes.length) ? state.refresh_modes : [60, 120];
    const shownMult = multDraft && multDraft[0] === (pick || 0) ? multDraft[1] : prof ? prof.mult : 2;
    const options = games.map((g) => ({ data: g.appid, label: running && running.appid === g.appid ? `${g.name} (running)` : g.name }));
    if (running && !games.some((g) => g.appid === running.appid)) options.unshift({ data: running.appid, label: `${running.name} (running)` });
    options.unshift({ data: 0, label: "Default (all games)" });

    let status = "No game is using frame generation";
    if (live && live.running) {
        status = live.active_mult
            ? `${live.width}×${live.height}: game ${live.real_fps} fps → screen ${live.out_fps} fps (x${live.active_mult})`
            : `${live.width}×${live.height}: paused, game ${live.real_fps} fps`;
    }

    return h(SP_REACT.Fragment, null,
        state && !state.installed && h(PanelSection, null,
            h(PanelSectionRow, null, h(Field, { label: "Layer not installed", description: "Reload the plugin; see the Decky log" }))),
        h(PanelSection, { title: "Game" },
            h(PanelSectionRow, null, h(DropdownItem, {
                label: "Game", rgOptions: options, selectedOption: pick, onChange: (o) => setPick(o.data),
            })),
            pick ? h(PanelSectionRow, null, h(ToggleField, {
                label: "Frame generation",
                description: note || (opts === null ? "Reading launch options…" : "Adds RP6FG=1 to the launch options; applies on next start"),
                checked: hasFG(opts), disabled: opts === null, onChange: toggleGame,
            })) : h(PanelSectionRow, null, h(Field, {
                label: "Defaults", description: "Settings below apply to every game without its own", bottomSeparator: "none",
            })),
            prof && h(PanelSectionRow, null, h(DropdownItem, {
                label: "Target refresh",
                description: "The screen switches to this rate while the game runs; frames are generated up to it",
                rgOptions: modes.map((hz) => ({ data: hz, label: `${hz} Hz` })), selectedOption: prof.target_hz,
                onChange: (o) => setProfile("target_hz", o.data),
            })),
            prof && h(PanelSectionRow, null, h(ToggleField, {
                label: "Adaptive",
                description: `Generates as many frames as the game needs to reach ${prof.target_hz} fps: ` +
                    `x2 at ${fps(prof.target_hz, 2)} fps, x3 at ${fps(prof.target_hz, 3)}, x4 at ${fps(prof.target_hz, 4)}`,
                checked: prof.adaptive, onChange: (v) => setProfile("adaptive", v),
            })),
            prof && h(PanelSectionRow, null, h(SliderField, {
                label: prof.adaptive ? `Max multiplier x${shownMult}` : `Multiplier x${shownMult}`,
                description: prof.adaptive ? "Upper limit; more keeps slower games smooth, each step adds a swapchain image (one frame of queue latency)"
                    : `Best when the game is capped at ${fps(prof.target_hz, shownMult)} fps`,
                value: shownMult, min: 2, max: 4, step: 1, notchCount: 3, notchTicksVisible: true,
                notchLabels: [{ notchIndex: 0, label: "x2", value: 2 }, { notchIndex: 1, label: "x3", value: 3 }, { notchIndex: 2, label: "x4", value: 4 }],
                onChange: setMult,
            }))),
        h(PanelSection, { title: "Live" },
            h(PanelSectionRow, null, h(Field, { label: "Status", description: status, bottomSeparator: "none" })),
            live && live.running && live.active_mult > 0 && h(PanelSectionRow, null, h(Field, {
                label: "Layer CPU per frame",
                description: `${live.cpu_us} µs own + ${live.submit_us} µs driver submit`, bottomSeparator: "none",
            })),
            live && h(PanelSectionRow, null, h(ToggleField, {
                label: "Generate frames", description: "Off passes the game's frames straight through",
                checked: live.enabled, onChange: (v) => call("set_enabled", v, pick || 0)
                    .then((s) => { if (alive.current && s.profile_appid === (pickRef.current || 0)) setState(s); }).catch(() => { }),
            }))));
}

const icon = h("svg", {
    xmlns: "http://www.w3.org/2000/svg", width: "24", height: "24", viewBox: "0 0 24 24", fill: "none",
    stroke: "currentColor", strokeWidth: "2", strokeLinecap: "round", strokeLinejoin: "round",
}, h("rect", { x: "2", y: "6", width: "8", height: "12", rx: "1" }), h("rect", { x: "14", y: "6", width: "8", height: "12", rx: "1" }),
    h("path", { d: "M10 12h4" }), h("path", { d: "M12 10l2 2-2 2" }));

const index = () => ({
    name: manifest.name,
    titleView: h("div", { className: DFL.staticClasses && DFL.staticClasses.Title }, manifest.name),
    content: h(Content, null),
    icon,
});

export { index as default };
