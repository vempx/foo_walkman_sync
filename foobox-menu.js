// Included by the foobox playlist manager after installing its menu adapter.
function walkmanMenu(menu, playlistIndex) {
    try {
        return buildWalkmanMenu(menu, playlistIndex);
    } catch (error) {
        console.log("Walkman Sync: " + error);
        return null;
    }
}

function buildWalkmanMenu(menu, playlistIndex) {
    const dir = fb.ProfilePath + "walkman-sync\\";
    const selection = brw.actionRows.map(row => brw.rows[row].idx);
    const indices = selection.indexOf(playlistIndex) >= 0 ? selection : [playlistIndex];
    const nonce = String(Date.now()) + "-" + String(Math.random());
    function call(action, playlistId, deviceId) {
        utils.WriteTextFile(dir + "panel-request.txt", [action + "2", nonce, indices.join(","), "", playlistId, deviceId].join("\n"));
        if (!fb.RunMainMenuCommand("文件/Walkman Sync Panel")) fb.RunMainMenuCommand("File/Walkman Sync Panel");
    }
    if (!utils.CheckComponent("foo_walkman_sync")) return null;
    call("list", "", "");
    const lines = utils.ReadTextFile(dir + "panel-response.txt", 65001).split(/\r?\n/);
    if (lines[0] !== nonce) return null;
    const commands = {};
    const group = window.CreatePopupMenu();
    let next = 60000;
    for (const line of lines.slice(2)) {
        if (!line) continue;
        const [device, label, online, subscribed] = line.split("\t");
        const popup = window.CreatePopupMenu();
        const send = next++, toggle = next++;
        popup.AppendMenuItem(online === "1" ? MF_STRING : MF_GRAYED, send, "发送");
        popup.AppendMenuItem(MF_STRING, toggle, "订阅");
        popup.CheckMenuItem(toggle, subscribed === "1");
        popup.AppendTo(group, MF_STRING, label.replace(/&/g, "&&"));
        commands[send] = () => call("send", lines[1], device);
        commands[toggle] = () => call(subscribed === "1" ? "unsubscribe" : "subscribe", lines[1], device);
    }
    if (next > 60000) group.AppendTo(menu, MF_STRING, "Walkman Sync");
    return commands;
}
