function (command, selectedId, epoch) {
    "use strict";
    // Executed only in the owned main frame's isolated world. Never inspect auth
    // fields, page globals, tokens, or network responses. Selectors are from the
    // legacy portal's shipped Fluent UI, not guessed positions or React internals.
    const unsupported = () => ({state: "unsupported"});
    if (window !== window.top || location.origin !== "https://client.wvd.microsoft.com"
        || location.pathname !== "/arm/webclient/index.html") return unsupported();
    if (document.readyState !== "complete") return {state: "waiting"};
    const visible = element => element && element.isConnected
        && element.getClientRects().length > 0
        && getComputedStyle(element).visibility !== "hidden";
    const enabled = element => visible(element) && !element.disabled
        && element.getAttribute("aria-disabled") !== "true";
    const one = (selector, root = document) => {
        const matches = [...root.querySelectorAll(selector)].filter(visible);
        return matches.length === 1 ? matches[0] : null;
    };
    const cleanName = text => typeof text === "string" && text.trim().length > 0
        && text.trim().length <= 160 && !/[\p{Cc}\u2028\u2029]/u.test(text);
    const shell = one(".tenants-container");
    if (!shell || document.querySelector(".tenants-loading, .tenant-resources-loading, .signout-page"))
        return {state: "waiting"};
    const state = globalThis.__omawin365Acquisition ||= {
        phase: "settings", radioClicked: false, nextId: 0, resources: new Map(), clicked: false
    };
    if (state.clicked) return {state: "downloading"};
    const section = one('.settings-item[aria-labelledby="launchmethod"]');
    for (const dialog of document.querySelectorAll('[role="dialog"], [role="alertdialog"]')) {
        if (visible(dialog) && (!section || !dialog.contains(section))) return unsupported();
    }
    // Reveal collapsed groups before counting. Never mistake group headers or
    // folder controls for resource tiles, or assume the first tile is the only PC.
    const collapsed = [...shell.querySelectorAll('.tenant-header-expand-button[aria-expanded="false"]')]
        .filter(enabled);
    if (collapsed.length) {
        if (command === "select") return unsupported();
        collapsed[0].click();
        return {state: "working"};
    }
    if (!section) {
        if (command === "select") return unsupported();
        if (state.phase === "settings") {
            const settings = [...document.querySelectorAll('.navactions-desktop button.navbar-icon')]
                .filter(button => enabled(button) && button.querySelector('[data-icon-name="Settings"]'));
            if (settings.length === 1) {
                state.phase = "panel";
                settings[0].click();
            } else {
                const overflow = one('.navactions-mobile button.navbar-icon[aria-haspopup="true"]');
                if (!enabled(overflow)) return unsupported();
                state.phase = "menu";
                overflow.click();
            }
        } else if (state.phase === "menu") {
            // This overflow item has no stable ID/icon in the shipped portal.
            // Only the observed English accessible label is supported here.
            if (!/^en(?:-|$)/i.test(document.documentElement.lang)) return unsupported();
            const settings = one('[role="menu"] [role="menuitem"][aria-label="Settings"]');
            if (settings) {
                if (!enabled(settings)) return unsupported();
                state.phase = "panel";
                settings.click();
            }
        }
        return {state: "working"};
    }
    const radios = [...section.querySelectorAll('input[type="radio"]')];
    const download = radios.find(input => input.id === input.name + "-download");
    const browser = radios.find(input => input.id === input.name + "-browser");
    // Fluent visually hides the native input, so visibility belongs to its
    // rendered choice-field label. Disabled/admin-managed choices are not changed.
    const radioVisible = input => input && visible(input.closest('.ms-ChoiceField'));
    if (radios.length !== 2 || !download || !browser || download.name !== browser.name
        || download.disabled || browser.disabled
        || !radioVisible(download) || !radioVisible(browser)) return unsupported();
    if (!download.checked || browser.checked) {
        if (command === "select" || state.radioClicked) return unsupported();
        state.radioClicked = true;
        download.click();
        return {state: "working"};
    }
    // The dedicated key is written synchronously by the same reducer as the
    // radio change. The combined client settings blob is debounced, so not proof.
    if (localStorage.getItem("RdWebAppSettings::resourceLaunchMethod") !== '"nativeclient"')
        return unsupported();
    const candidates = [...shell.querySelectorAll(
        '.tenant-resources[role="listbox"] button.tenant-resources-btn[role="option"], '
        + '.tenant-resources[role="listbox"] .list-view[role="option"]')].filter(visible);
    // Folder contents are not enumerated here. Even one visible PC beside a
    // folder is incomplete discovery, so never auto-select it.
    if (candidates.some(element => element.querySelector(
        '.tile-view-thumbnail--icon, .list-view-thumbnail--icon'))) return unsupported();
    if (candidates.length > 128) return unsupported();
    const resources = [];
    for (const element of candidates) {
        if (!enabled(element) || element.closest('.tile-error, .list-view-error')) continue;
        const name = element.getAttribute("aria-label");
        const group = element.closest('.tenant-resources[role="listbox"]').getAttribute("aria-label") || "";
        if (!cleanName(name) || (group && !cleanName(group))) return unsupported();
        let entry = state.resources.get(element);
        if (!entry || entry.name !== name.trim() || entry.group !== group.trim()) {
            entry = {id: epoch + "-resource-" + ++state.nextId, name: name.trim(), group: group.trim()};
            state.resources.set(element, entry);
        }
        resources.push({...entry, element});
    }
    for (const element of state.resources.keys()) {
        if (!resources.some(resource => resource.element === element)) state.resources.delete(element);
    }
    if (command === "select") {
        const chosen = resources.filter(resource => resource.id === selectedId);
        if (chosen.length !== 1) return unsupported();
        const panel = section.closest('.sidepanel');
        const close = panel && one('button.close-button', panel);
        if (!enabled(close)) return unsupported();
        // Check mode, close the panel, and activate exactly this still-connected
        // tile in one synchronous evaluation: no asynchronous gap or second click.
        state.clicked = true;
        close.click();
        if (!chosen[0].element.isConnected
            || localStorage.getItem("RdWebAppSettings::resourceLaunchMethod") !== '"nativeclient"')
            return unsupported();
        chosen[0].element.click();
        return {state: "downloading"};
    }
    if (!resources.length) return {state: "empty"};
    return {state: "resources", resources: resources.map(({id, name, group}) => ({id, name, group}))};
}
