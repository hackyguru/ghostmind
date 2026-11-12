// Ghostmind — anonymous therapy on Logos.
//
// The core (ghostmind_core) owns the wallet, the registry reads and the encrypted
// conversations; this file polls its state once a second and draws it. Every
// action is a method on the core that returns {"ok":…} at once and, for chain
// work, a job id whose progress shows up in state().jobs.
//
// Two modes, chosen on the welcome screen and remembered by the core:
//   client     Find a therapist · Messages · Wallet
//   therapist  My practice · Clients · Wallet (+ Licence checks / Council)
// A client never sees therapist screens and vice versa. Anyone with a
// listing, a verifier seat or the council is in therapist mode.
// Logos terms (LEZ, private account, shield, faucet, recovery phrase) stay as
// labels, with a plain-English line next to them.
import QtQuick
import QtQuick.Controls
import QtQuick.Layouts

import Logos.Theme
import Logos.Controls

Item {
    id: root
    implicitWidth: 1000
    implicitHeight: 760

    readonly property string coreId: "ghostmind_core"

    property var st: ({})
    property string lastError: ""
    property string copied: ""
    property int tab: 0
    property string localMode: ""
    property string selectedConv: ""
    property bool editingProfile: false
    property bool showRecovery: false
    property string recoveryWords: ""
    property int ratingScore: 0
    property bool profileLoaded: false
    property bool showAdvanced: false
    property bool showHowItWorks: false
    property bool confirmDelete: false
    property string search: ""
    property bool verifiedOnly: false

    readonly property bool ready: st.phase === "ready"
    readonly property var net: st.network || ({})
    readonly property var program: st.program || ({})
    readonly property var me: st.me || ({})
    readonly property var mine: me.therapist || null
    readonly property var therapists: st.therapists || []
    readonly property var verifiers: st.verifiers || []
    readonly property var activeVerifiers: verifiers.filter(function (v) { return v.active; })
    // client | therapist | "" (not chosen yet)
    readonly property string mode: (mine || me.isVerifier === true || me.isCouncil === true) ? "therapist" : (localMode || me.mode || "")
    readonly property bool pro: mode === "therapist"
    readonly property bool modeLocked: !!mine || me.isVerifier === true || me.isCouncil === true
    // In client mode only your own therapy chats; in therapist mode everything else.
    readonly property var conversations: (st.conversations || []).filter(function (c) {
        var asClient = c.role === "client" && c.purpose === "therapy";
        return pro ? !asClient : asClient;
    }).sort(function (a, b) { return b.lastMs - a.lastMs; })
    readonly property var jobs: st.jobs || []
    readonly property int unreadTotal: conversations.reduce(function (n, c) { return n + (c.unread || 0); }, 0)
    readonly property var current: conversations.filter(function (c) { return c.id === selectedConv; })[0] || null
    readonly property bool isCouncil: me.isCouncil === true
    readonly property bool isVerifier: me.isVerifier === true
    readonly property bool showCouncil: isCouncil || program.hasCouncil === false
    readonly property var activeJobs: jobs.filter(function (j) { return j.status !== "done" && j.status !== "failed"; })
    readonly property var bannerJobs: activeJobs.filter(function (j) { return !(tab === 1 && j.conversation && j.conversation === selectedConv); })
    readonly property string feeText: mine && mine.ticketFee ? mine.ticketFee + " LGO" : "2% of your price (at least 0.01 LGO)"
    readonly property bool connected: ready && net.online === true && net.messaging === 2
    readonly property bool showWelcome: ready && mode === ""
    readonly property var pages: pro ? ["practice", "messages", "wallet"].concat(isVerifier ? ["checks"] : []).concat(showCouncil ? ["council"] : []) : ["find", "messages", "wallet"]
    readonly property string page: pages[Math.max(0, Math.min(tab, pages.length - 1))] || pages[0]
    function pageTitle(p) {
        switch (p) {
        case "find":
            return "Find a therapist";
        case "messages":
            return unreadTotal > 0 ? (pro ? "Clients (" : "Messages (") + unreadTotal + ")" : (pro ? "Clients" : "Messages");
        case "wallet":
            return "Wallet";
        case "practice":
            return "My practice";
        case "checks":
            return "Licence checks";
        case "council":
            return "Council";
        }
        return p;
    }
    function chooseMode(m) {
        localMode = m;
        tab = 0;
        selectedConv = "";
        callArgs("setMode", [m]);
    }

    // Therapists a client should see: listed only, verified first, then by rating.
    readonly property var directory: therapists.filter(function (t) {
        if (t.status !== 0 || t.mine === true)
            return false;
        if (verifiedOnly && t.verified !== true)
            return false;
        var q = search.trim().toLowerCase();
        if (!q.length)
            return true;
        return ((t.name || "") + " " + (t.specialties || "") + " " + (t.bio || "")).toLowerCase().indexOf(q) >= 0;
    }).sort(function (a, b) {
        if ((a.verified === true) !== (b.verified === true))
            return a.verified === true ? -1 : 1;
        if ((b.rating || 0) !== (a.rating || 0))
            return (b.rating || 0) - (a.rating || 0);
        return (b.ratingCount || 0) - (a.ratingCount || 0);
    })

    // ── Logos bridge ─────────────────────────────────────────────────
    //   logos.callModule(id, method, [])            — synchronous, no-arg only
    //   logos.callModuleAsync(id, method, args, cb) — anything with arguments
    function call(method) {
        if (typeof logos === "undefined" || !logos.callModule)
            return null;
        return logos.callModule(coreId, method, []);
    }
    function callArgs(method, args, cb) {
        if (typeof logos === "undefined" || !logos.callModuleAsync) {
            lastError = "Ghostmind can't reach its background service. Try reopening the app.";
            return;
        }
        logos.callModuleAsync(coreId, method, args, function (raw) {
            var r = unwrap(raw, null);
            if (r && r.ok === false)
                lastError = friendlyError(r.error || "Something went wrong.");
            refresh();
            if (cb)
                cb(r);
        });
    }
    // The bridge JSON-encodes the module's return value, so a JSON-returning
    // method arrives double-encoded: parse while it's still a string.
    function unwrap(raw, def) {
        if (raw === null || raw === undefined)
            return def;
        var v = raw;
        for (var i = 0; i < 3 && typeof v === "string"; ++i) {
            try {
                v = JSON.parse(v);
            } catch (e) {
                return (i === 0) ? def : v;
            }
        }
        return v;
    }
    function refresh() {
        var s = unwrap(call("state"), null);
        if (s && typeof s === "object") {
            st = s;
            for (var i = 0; i < jobs.length; ++i)
                if (jobs[i].status === "failed" && Date.now() - jobs[i].finished < 1500)
                    lastError = friendlyError(jobs[i].error);
            if (!profileLoaded && mine) {
                nameField.text = mine.name;
                specField.text = mine.specialties;
                bioField.text = mine.bio;
                priceField.text = mine.price;
                profileLoaded = true;
            }
        }
    }
    function friendlyError(e) {
        var s = (e || "").toString();
        var l = s.toLowerCase();
        if (l.indexOf("faucet") >= 0)
            return "Couldn't get test LGO from the faucet just now. It's shared by everyone on the testnet, so it gets busy. Try again in a minute.";
        if (l.indexOf("did not finalize") >= 0 || l.indexOf("timed out") >= 0 || l.indexOf("timeout") >= 0)
            return "The LEZ testnet is slow right now and didn't confirm in time. Nothing was lost. Try again in a minute.";
        if (l.indexOf("insufficient") >= 0 || l.indexOf("not enough") >= 0)
            return "There isn't enough LGO in your wallet for that. Get test LGO in the Wallet tab and try again.";
        if (l.indexOf("hasn't caught up") >= 0)
            return "Your LGO was moved to your private account, but it hasn't shown up yet. Try paying again in a minute.";
        return s;
    }
    function openConv(id) {
        selectedConv = id;
        tab = 1;
        ratingScore = 0;
        confirmDelete = false;
        callArgs("markRead", [id]);
    }
    function startChat(account) {
        callArgs("openConversation", [account], function (r) {
            if (r && r.ok)
                root.openConv(r.conversation);
        });
    }
    function therapistOf(address) {
        for (var i = 0; i < therapists.length; ++i)
            if (therapists[i].account === address)
                return therapists[i];
        return null;
    }
    function verifierName(address) {
        for (var i = 0; i < verifiers.length; ++i)
            if (verifiers[i].account === address)
                return verifiers[i].name;
        return shortAddr(address);
    }
    function shortAddr(a) {
        a = a || "";
        return a.length > 12 ? a.substring(0, 4) + "…" + a.substring(a.length - 4) : a;
    }
    function whenOf(ms) {
        if (!ms)
            return "";
        var s = Math.max(0, Math.floor((Date.now() - ms) / 1000));
        if (s < 60)
            return "just now";
        if (s < 3600)
            return Math.round(s / 60) + " min ago";
        if (s < 86400)
            return Math.round(s / 3600) + " h ago";
        return new Date(ms).toLocaleDateString(Qt.locale("en_US"), "d MMM yyyy");
    }
    function timeOf(ms) {
        return ms ? new Date(ms).toLocaleTimeString(Qt.locale("en_US"), "HH:mm") : "";
    }
    function initials(name) {
        var n = (name || "").trim();
        if (!n.length)
            return "?";
        var parts = n.split(/\s+/).filter(function (p) { return /[A-Za-z0-9]/.test(p.charAt(0)); });
        if (!parts.length)
            return n.substring(0, 1).toUpperCase();
        if (parts.length > 1 && /^(dr|mr|mrs|ms|prof)\.?$/i.test(parts[0]))
            parts.shift();
        return (parts.length > 1 ? parts[0].charAt(0) + parts[parts.length - 1].charAt(0) : parts[0].substring(0, 1)).toUpperCase();
    }
    function hueFor(seed) {
        var h = 0;
        var s = seed || "";
        for (var i = 0; i < s.length; i++)
            h = (h * 31 + s.charCodeAt(i)) >>> 0;
        return (h % 360) / 360;
    }
    function stars(r) {
        var n = Math.round(r || 0);
        var s = "";
        for (var i = 0; i < 5; ++i)
            s += i < n ? "★" : "☆";
        return s;
    }
    function ratingLine(t) {
        if (!t || !(t.ratingCount > 0))
            return "New on Ghostmind";
        return Number(t.rating).toFixed(1) + " · " + t.ratingCount + (t.ratingCount === 1 ? " review" : " reviews");
    }
    function specialtiesOf(t) {
        return ((t && t.specialties) || "").split(/[,;·]/).map(function (s) { return s.trim(); }).filter(function (s) { return s.length > 0; }).slice(0, 6);
    }
    function credentialLine(t) {
        if (!t || !t.credential)
            return "";
        var c = t.credential;
        var who = c.verifierName || verifierName(c.verifier);
        return "Licence checked by " + who + (c.jurisdiction ? " · " + c.jurisdiction : "") + (c.expires ? (c.expired ? " · expired " : " · valid until ") + c.expires : "");
    }
    function statusText(t) {
        if (!t)
            return "";
        if (t.status === 1)
            return "Suspended";
        if (t.status === 2)
            return "Hidden after reports";
        return "Listed";
    }
    function jobVerb(j) {
        switch (j.kind) {
        case "pay":
            return "Sending your private payment";
        case "shield":
            return "Shielding LGO into your private account";
        case "gas":
            return "Getting test LGO from the faucet";
        case "rate":
            return "Posting your anonymous review";
        case "ticket":
            return "Confirming a client's payment";
        case "register":
            return "Publishing your listing";
        case "update":
            return "Saving your listing";
        case "verify":
            return "Recording the licence check";
        case "revoke":
            return "Withdrawing the licence check";
        default:
            return j.label;
        }
    }
    function jobPhase(j) {
        if (j.status === "proving")
            return "preparing it privately on this computer. This takes a few minutes";
        if (j.status === "confirming")
            return "waiting for the LEZ testnet to confirm (about a minute)";
        if (j.status === "queued")
            return "starting";
        return "";
    }
    function jobText(j) {
        if (!j)
            return "";
        var p = jobPhase(j);
        return jobVerb(j) + (p.length ? " · " + p : "…");
    }
    function sendCurrent() {
        if (!current)
            return;
        var t = composer.text.trim();
        if (!t.length || !ready)
            return;
        callArgs("sendMessage", [current.id, t], function (r) {
            if (r && r.ok)
                composer.text = "";
        });
    }
    function scrollToEnd() {
        Qt.callLater(function () {
            var f = scroller.contentItem;
            if (f && f.contentHeight > f.height)
                f.contentY = f.contentHeight - f.height;
        });
    }

    readonly property int currentMsgCount: current && current.messages ? current.messages.length : 0
    onCurrentMsgCountChanged: if (tab === 1 && current) scrollToEnd()
    onSelectedConvChanged: if (selectedConv.length) scrollToEnd()

    TextEdit {
        id: clip
        visible: false
    }
    function copy(t, key) {
        clip.text = t;
        clip.selectAll();
        clip.copy();
        copied = key;
        copyTimer.restart();
    }
    Timer {
        id: copyTimer
        interval: 1600
        onTriggered: root.copied = ""
    }
    Timer {
        interval: 1000
        running: true
        repeat: true
        triggeredOnStart: true
        onTriggered: root.refresh()
    }

    Gradient {
        id: accentGrad
        GradientStop {
            position: 0.0
            color: "#F28E6B"
        }
        GradientStop {
            position: 1.0
            color: "#E1613A"
        }
    }

    // ── reusable pieces ───────────────────────────────────────────────
    component Card: Rectangle {
        id: card
        default property alias content: inner.data
        property int pad: Theme.spacing.xlarge
        Layout.fillWidth: true
        implicitHeight: inner.implicitHeight + 2 * pad
        color: Theme.palette.backgroundElevated
        radius: Theme.spacing.radiusXlarge
        border.color: Theme.palette.borderSecondary
        ColumnLayout {
            id: inner
            anchors.fill: parent
            anchors.margins: card.pad
            spacing: Theme.spacing.medium
        }
    }

    component Title: LogosText {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.palette.text
        font.pixelSize: Theme.typography.subtitleText
        font.weight: Theme.typography.weightBold
    }

    component Heading: LogosText {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.palette.text
        font.pixelSize: Theme.typography.panelTitleText
        font.weight: Theme.typography.weightBold
    }

    component Caption: LogosText {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.palette.textTertiary
        font.pixelSize: Theme.typography.secondaryText
        lineHeight: 1.15
    }

    component Body: LogosText {
        Layout.fillWidth: true
        wrapMode: Text.WordWrap
        color: Theme.palette.textSecondary
        font.pixelSize: Theme.typography.primaryText
        lineHeight: 1.2
    }

    component Chip: Rectangle {
        property string text: ""
        property color tint: Theme.palette.primary
        implicitWidth: chipLbl.implicitWidth + 16
        implicitHeight: 22
        radius: 11
        color: Theme.colors.getColor(tint, 0.16)
        LogosText {
            id: chipLbl
            anchors.centerIn: parent
            text: parent.text
            color: parent.tint
            font.pixelSize: Theme.typography.secondaryText
            font.weight: Theme.typography.weightBold
        }
    }

    component Tag: Rectangle {
        property string text: ""
        implicitWidth: tagLbl.implicitWidth + 18
        implicitHeight: 24
        radius: 12
        color: Theme.palette.backgroundSecondary
        border.color: Theme.palette.borderSecondary
        LogosText {
            id: tagLbl
            anchors.centerIn: parent
            text: parent.text
            color: Theme.palette.textSecondary
            font.pixelSize: Theme.typography.secondaryText
        }
    }

    component Toggle: Rectangle {
        id: tg
        property string text: ""
        property bool on: false
        signal clicked()
        implicitWidth: tgLbl.implicitWidth + 28
        implicitHeight: 32
        radius: 16
        color: on ? Theme.colors.getColor(Theme.palette.primary, 0.18) : "transparent"
        border.color: on ? Theme.palette.primary : Theme.palette.borderSecondary
        LogosText {
            id: tgLbl
            anchors.centerIn: parent
            text: tg.text
            color: tg.on ? Theme.palette.primary : Theme.palette.textSecondary
            font.pixelSize: Theme.typography.secondaryText
            font.weight: Theme.typography.weightBold
        }
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: tg.clicked()
        }
    }

    component Pill: Rectangle {
        id: pill
        property string text: ""
        property bool can: true
        property bool busy: false
        property bool secondary: false
        property bool danger: false
        signal clicked()
        implicitWidth: pillLbl.implicitWidth + (busy ? 62 : 40)
        implicitHeight: 40
        radius: 20
        gradient: can && !secondary && !danger ? accentGrad : null
        color: danger ? Theme.colors.getColor(Theme.palette.error, 0.18) : Theme.palette.backgroundSecondary
        border.color: danger ? Theme.palette.error : secondary ? Theme.palette.borderSecondary : "transparent"
        opacity: can ? 1 : 0.45
        RowLayout {
            anchors.centerIn: parent
            spacing: Theme.spacing.small
            LogosSpinner {
                visible: pill.busy
                running: visible
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
                ringColor: pill.secondary || pill.danger ? Theme.palette.text : "#ffffff"
                thickness: 2
                dotSize: 4
            }
            LogosText {
                id: pillLbl
                text: pill.text
                color: pill.danger ? Theme.palette.error : pill.secondary ? Theme.palette.text : "#ffffff"
                font.pixelSize: Theme.typography.primaryText
                font.weight: Theme.typography.weightBold
            }
        }
        MouseArea {
            anchors.fill: parent
            enabled: pill.can && !pill.busy
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: pill.clicked()
        }
    }

    component Link: LogosText {
        signal clicked()
        color: Theme.palette.primary
        font.pixelSize: Theme.typography.secondaryText
        font.weight: Theme.typography.weightBold
        MouseArea {
            anchors.fill: parent
            cursorShape: Qt.PointingHandCursor
            onClicked: parent.clicked()
        }
    }

    component CopyRow: RowLayout {
        id: cr
        property string label: ""
        property string value: ""
        property string hint: ""
        Layout.fillWidth: true
        spacing: Theme.spacing.medium
        ColumnLayout {
            Layout.fillWidth: true
            spacing: 1
            LogosText {
                text: cr.label
                color: Theme.palette.textSecondary
                font.pixelSize: Theme.typography.secondaryText
                font.weight: Theme.typography.weightBold
            }
            LogosText {
                Layout.fillWidth: true
                text: cr.value || "—"
                elide: Text.ElideMiddle
                font.family: "Menlo"
                color: Theme.palette.text
                font.pixelSize: Theme.typography.secondaryText
            }
            Caption {
                visible: cr.hint.length > 0
                text: cr.hint
            }
        }
        Link {
            text: root.copied === cr.label ? "Copied ✓" : "Copy"
            visible: cr.value.length > 0
            onClicked: root.copy(cr.value, cr.label)
        }
    }

    component Field: LogosTextField {
        Layout.fillWidth: true
        implicitHeight: 42
    }

    component FieldLabel: LogosText {
        Layout.topMargin: 2
        color: Theme.palette.textSecondary
        font.pixelSize: Theme.typography.secondaryText
        font.weight: Theme.typography.weightBold
    }

    component Avatar: Rectangle {
        property string seed: ""
        property string name: ""
        property int size: 44
        implicitWidth: size
        implicitHeight: size
        radius: size / 2
        color: Qt.hsla(root.hueFor(seed), 0.42, 0.42, 1)
        LogosText {
            anchors.centerIn: parent
            text: root.initials(parent.name)
            color: "#ffffff"
            font.pixelSize: Math.round(parent.size * 0.34)
            font.weight: Theme.typography.weightBold
        }
    }

    component IconBubble: Rectangle {
        property string glyph: ""
        property color glyphColor: Theme.palette.primary
        implicitWidth: 40
        implicitHeight: 40
        radius: 12
        color: Theme.colors.getColor(Theme.palette.primary, 0.14)
        LogosText {
            anchors.centerIn: parent
            text: parent.glyph
            color: parent.glyphColor
            font.pixelSize: 18
            font.weight: Theme.typography.weightBold
        }
    }

    component Step: RowLayout {
        id: stp
        property int n: 1
        property string text: ""
        property bool done: false
        property bool active: false
        spacing: Theme.spacing.small
        Rectangle {
            implicitWidth: 24
            implicitHeight: 24
            radius: 12
            color: stp.done ? Theme.palette.success : stp.active ? Theme.palette.primary : "transparent"
            border.color: stp.done ? Theme.palette.success : stp.active ? Theme.palette.primary : Theme.palette.borderSecondary
            LogosText {
                anchors.centerIn: parent
                text: stp.done ? "✓" : stp.n
                color: stp.done || stp.active ? "#ffffff" : Theme.palette.textTertiary
                font.pixelSize: Theme.typography.secondaryText
                font.weight: Theme.typography.weightBold
            }
        }
        LogosText {
            text: stp.text
            color: stp.done || stp.active ? Theme.palette.text : Theme.palette.textTertiary
            font.pixelSize: Theme.typography.secondaryText
            font.weight: stp.active ? Theme.typography.weightBold : Theme.typography.weightRegular
        }
    }

    component StepLine: Rectangle {
        Layout.fillWidth: true
        Layout.minimumWidth: 16
        implicitHeight: 1
        color: Theme.palette.borderSecondary
    }

    component Notice: Rectangle {
        id: nt
        default property alias content: ntInner.data
        property color tint: Theme.palette.info
        Layout.fillWidth: true
        implicitHeight: ntInner.implicitHeight + 2 * Theme.spacing.medium
        radius: Theme.spacing.radiusLarge
        color: Theme.colors.getColor(tint, 0.10)
        border.color: Theme.colors.getColor(tint, 0.35)
        RowLayout {
            id: ntInner
            anchors.fill: parent
            anchors.margins: Theme.spacing.medium
            spacing: Theme.spacing.medium
        }
    }

    component CrisisNote: Notice {
        tint: Theme.palette.warning
        LogosText {
            text: "♥"
            color: Theme.palette.warning
            font.pixelSize: 16
            Layout.alignment: Qt.AlignTop
        }
        Caption {
            color: Theme.palette.textSecondary
            text: "Ghostmind isn't an emergency service. If you're in danger or thinking about harming yourself, call your local emergency number now, or find a free crisis line near you at <a href=\"https://findahelpline.com\">findahelpline.com</a>."
            textFormat: Text.StyledText
            linkColor: Theme.palette.primary
            onLinkActivated: function (link) { Qt.openUrlExternally(link); }
        }
    }

    component TherapistCard: Card {
        id: tcard
        property var t: ({ account: "", name: "", specialties: "", bio: "", price: "", status: 0, rating: 0, ratingCount: 0, reports: 0, tickets: 0, reviews: 0 })
        property bool preview: false
        property bool showReviews: false
        property bool bioOpen: false
        pad: Theme.spacing.xlarge
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.large
            Avatar {
                seed: tcard.t.account
                name: tcard.t.name
                size: 56
                Layout.alignment: Qt.AlignTop
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 6
                RowLayout {
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small
                    LogosText {
                        text: tcard.t.name
                        color: Theme.palette.text
                        font.pixelSize: Theme.typography.subtitleText
                        font.weight: Theme.typography.weightBold
                    }
                    Chip {
                        visible: tcard.t.verified === true && !(tcard.t.credential && tcard.t.credential.expired)
                        text: "✓ Verified"
                        tint: Theme.palette.success
                    }
                    Chip {
                        visible: tcard.t.status !== 0
                        text: root.statusText(tcard.t)
                        tint: Theme.palette.error
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    ColumnLayout {
                        spacing: 0
                        LogosText {
                            Layout.alignment: Qt.AlignRight
                            text: tcard.t.price + " LGO"
                            color: Theme.palette.text
                            font.pixelSize: Theme.typography.subtitleText
                            font.weight: Theme.typography.weightBold
                        }
                        LogosText {
                            Layout.alignment: Qt.AlignRight
                            text: "per session"
                            color: Theme.palette.textTertiary
                            font.pixelSize: Theme.typography.secondaryText
                        }
                    }
                }
                RowLayout {
                    spacing: 6
                    LogosText {
                        text: tcard.t.ratingCount > 0 ? root.stars(tcard.t.rating) : "★"
                        color: tcard.t.ratingCount > 0 ? "#F5B343" : Theme.palette.textMuted
                        font.pixelSize: Theme.typography.primaryText
                    }
                    LogosText {
                        text: root.ratingLine(tcard.t)
                        color: Theme.palette.textSecondary
                        font.pixelSize: Theme.typography.secondaryText
                    }
                }
                Caption {
                    visible: tcard.t.verified === true
                    text: root.credentialLine(tcard.t)
                    color: tcard.t.credential && tcard.t.credential.expired ? Theme.palette.error : Theme.palette.textTertiary
                }
                Caption {
                    visible: tcard.t.verified !== true
                    text: "Licence not checked yet"
                }
                Flow {
                    Layout.fillWidth: true
                    Layout.topMargin: 2
                    spacing: 6
                    visible: root.specialtiesOf(tcard.t).length > 0
                    Repeater {
                        model: root.specialtiesOf(tcard.t)
                        delegate: Tag {
                            required property string modelData
                            text: modelData
                        }
                    }
                }
                Body {
                    Layout.topMargin: 2
                    visible: (tcard.t.bio || "").length > 0
                    text: tcard.t.bio || ""
                    maximumLineCount: tcard.bioOpen ? 40 : 3
                    elide: Text.ElideRight
                }
                Link {
                    visible: !tcard.bioOpen && (tcard.t.bio || "").length > 220
                    text: "Read more"
                    onClicked: tcard.bioOpen = true
                }
                RowLayout {
                    Layout.fillWidth: true
                    Layout.topMargin: Theme.spacing.small
                    spacing: Theme.spacing.large
                    Pill {
                        visible: !tcard.preview
                        text: (tcard.t.name || "").length > 0 && tcard.t.name.length <= 22 ? "Message " + tcard.t.name : "Send a message"
                        can: root.ready && tcard.t.status === 0
                        onClicked: root.startChat(tcard.t.account)
                    }
                    Link {
                        visible: tcard.t.reviews > 0
                        text: tcard.showReviews ? "Hide reviews" : "Read reviews (" + tcard.t.reviews + ")"
                        onClicked: {
                            tcard.showReviews = !tcard.showReviews;
                            if (tcard.showReviews)
                                root.callArgs("loadReviews", [tcard.t.account]);
                        }
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                }
                ColumnLayout {
                    visible: tcard.showReviews
                    Layout.fillWidth: true
                    spacing: Theme.spacing.small
                    Caption {
                        visible: !tcard.t.reviewList || tcard.t.reviewList.length === 0
                        text: "Loading reviews…"
                    }
                    Repeater {
                        model: tcard.t.reviewList || []
                        delegate: Rectangle {
                            required property var modelData
                            Layout.fillWidth: true
                            implicitHeight: rv.implicitHeight + 2 * Theme.spacing.medium
                            radius: Theme.spacing.radiusLarge
                            color: Theme.palette.backgroundSecondary
                            ColumnLayout {
                                id: rv
                                anchors.fill: parent
                                anchors.margins: Theme.spacing.medium
                                spacing: 2
                                RowLayout {
                                    LogosText {
                                        text: root.stars(modelData.score)
                                        color: "#F5B343"
                                        font.pixelSize: Theme.typography.secondaryText
                                    }
                                    LogosText {
                                        text: "Anonymous client · " + root.whenOf(modelData.time * 1000)
                                        color: Theme.palette.textTertiary
                                        font.pixelSize: Theme.typography.secondaryText
                                    }
                                    Chip {
                                        visible: modelData.report === true
                                        text: "Reported a problem"
                                        tint: Theme.palette.error
                                    }
                                }
                                Body {
                                    visible: (modelData.text || "").length > 0
                                    text: modelData.text || ""
                                    font.pixelSize: Theme.typography.secondaryText
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Rectangle {
        anchors.fill: parent
        color: Theme.palette.background
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.margins: Theme.spacing.xlarge
        spacing: Theme.spacing.medium

        // ── header ──────────────────────────────────────────────────
        RowLayout {
            Layout.fillWidth: true
            spacing: Theme.spacing.medium
            Rectangle {
                implicitWidth: 40
                implicitHeight: 40
                radius: 12
                gradient: accentGrad
                LogosText {
                    anchors.centerIn: parent
                    text: "G"
                    color: "#ffffff"
                    font.pixelSize: 20
                    font.weight: Theme.typography.weightBold
                }
            }
            ColumnLayout {
                spacing: 0
                LogosText {
                    text: "Ghostmind"
                    color: Theme.palette.text
                    font.pixelSize: Theme.typography.panelTitleText
                    font.weight: Theme.typography.weightBold
                }
                LogosText {
                    text: root.pro ? "Your practice · clients stay anonymous, payments stay private" : "Talk to a verified therapist without giving your name"
                    color: Theme.palette.textTertiary
                    font.pixelSize: Theme.typography.secondaryText
                }
            }
            Item {
                Layout.fillWidth: true
            }
            Rectangle {
                id: statusPill
                implicitWidth: statusRow.implicitWidth + 24
                implicitHeight: 30
                radius: 15
                color: Theme.palette.backgroundElevated
                border.color: Theme.palette.borderSecondary
                RowLayout {
                    id: statusRow
                    anchors.centerIn: parent
                    spacing: 6
                    Rectangle {
                        implicitWidth: 8
                        implicitHeight: 8
                        radius: 4
                        color: root.st.phase === "error" ? Theme.palette.error : root.connected ? Theme.palette.success : Theme.palette.warning
                    }
                    LogosText {
                        text: root.st.phase === "error" ? "Wallet problem" : !root.ready ? "Starting…" : root.connected ? "Private & connected" : root.net.online !== true ? "Reconnecting to LEZ…" : "Connecting to messaging…"
                        color: Theme.palette.textSecondary
                        font.pixelSize: Theme.typography.secondaryText
                    }
                }
                HoverHandler {
                    id: statusHover
                }
                ToolTip.visible: statusHover.hovered
                ToolTip.delay: 300
                ToolTip.text: "LEZ testnet: " + (root.net.online ? "online, block " + root.net.block : "offline") + "\nLogos messaging: " + (root.net.messaging === 2 ? "connected" : root.net.messaging === 3 ? "down" : "connecting") + "\nMessages are end-to-end encrypted."
            }
        }

        // ── navigation ──────────────────────────────────────────────
        LogosTabBar {
            id: tabs
            visible: !root.showWelcome
            Layout.fillWidth: true
            currentIndex: root.tab
            onCurrentIndexChanged: root.tab = currentIndex
            Repeater {
                model: root.pages
                delegate: LogosTabButton {
                    required property string modelData
                    text: root.pageTitle(modelData)
                }
            }
        }

        // ── error ───────────────────────────────────────────────────
        Notice {
            visible: root.lastError.length > 0 || root.st.phase === "error"
            tint: Theme.palette.error
            Caption {
                text: root.st.phase === "error" ? "Ghostmind's wallet couldn't start: " + root.st.error : root.lastError
                color: Theme.palette.text
            }
            Link {
                visible: root.lastError.length > 0
                text: "Dismiss"
                Layout.alignment: Qt.AlignTop
                onClicked: root.lastError = ""
            }
        }

        // ── work in progress ────────────────────────────────────────
        Notice {
            visible: root.bannerJobs.length > 0 && !root.showWelcome
            tint: Theme.palette.primary
            LogosSpinner {
                running: visible
                Layout.preferredWidth: 14
                Layout.preferredHeight: 14
                thickness: 2
                dotSize: 4
            }
            Caption {
                color: Theme.palette.text
                text: root.bannerJobs.length ? root.jobText(root.bannerJobs[0]) + (root.bannerJobs.length > 1 ? "  (+" + (root.bannerJobs.length - 1) + " more)" : "") : ""
            }
        }

        // ── pages ───────────────────────────────────────────────────
        ScrollView {
            id: scroller
            Layout.fillWidth: true
            Layout.fillHeight: true
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                width: scroller.availableWidth
                spacing: Theme.spacing.large

                // ════ Welcome (first visit) ════
                ColumnLayout {
                    id: welcomeCol
                    visible: root.showWelcome
                    Layout.fillWidth: true
                    Layout.maximumWidth: 720
                    Layout.alignment: Qt.AlignHCenter
                    // Centre vertically in the visible area (minus the bottom spacer); scrolls when it doesn't fit.
                    Layout.topMargin: Math.max(Theme.spacing.large, (scroller.availableHeight - welcomeCol.implicitHeight - Theme.spacing.xxlarge) / 2)
                    spacing: Theme.spacing.xlarge

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing.small
                        LogosText {
                            Layout.fillWidth: true
                            horizontalAlignment: Text.AlignHCenter
                            wrapMode: Text.WordWrap
                            text: "Someone to talk to.\nNo name required."
                            color: Theme.palette.text
                            font.pixelSize: Theme.typography.titleText
                            font.weight: Theme.typography.weightBold
                        }
                        Body {
                            horizontalAlignment: Text.AlignHCenter
                            text: "Ghostmind connects you with licensed therapists. You don't sign up, you don't share your name, and nobody can see that you're paying for therapy."
                        }
                    }

                    Card {
                        Repeater {
                            model: [
                                { g: "◐", t: "You stay anonymous", d: "Therapists see a random nickname, never your name or account. Every message is end-to-end encrypted over Logos messaging." },
                                { g: "✓", t: "Therapists are checked", d: "A “Verified” badge means an accredited licensing body checked their licence. You can also read reviews from real paying clients." },
                                { g: "⦿", t: "Your payment stays private", d: "Sessions are paid in LGO from your private account on the Logos Execution Zone (LEZ). Nobody can see who paid whom, or how much." }
                            ]
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: Theme.spacing.large
                                IconBubble {
                                    glyph: modelData.g
                                    Layout.alignment: Qt.AlignTop
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Title {
                                        text: modelData.t
                                        font.pixelSize: Theme.typography.primaryText
                                    }
                                    Caption {
                                        text: modelData.d
                                    }
                                }
                            }
                        }
                    }

                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: Theme.spacing.medium
                        Pill {
                            text: "Find a therapist"
                            onClicked: {
                                root.chooseMode("client");
                            }
                        }
                        Pill {
                            text: "I'm a therapist"
                            secondary: true
                            onClicked: {
                                root.chooseMode("therapist");
                            }
                        }
                    }
                    CrisisNote {}
                }

                // ════ Find a therapist ════
                ColumnLayout {
                    visible: root.page === "find" && !root.showWelcome
                    Layout.fillWidth: true
                    spacing: Theme.spacing.large

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing.small
                        LogosTextField {
                            id: searchField
                            Layout.fillWidth: true
                            implicitHeight: 42
                            placeholderText: "Search by name or what you'd like help with, e.g. anxiety, grief"
                            onTextChanged: root.search = text
                        }
                        Toggle {
                            text: "Verified only"
                            on: root.verifiedOnly
                            onClicked: root.verifiedOnly = !root.verifiedOnly
                        }
                    }

                    // loading
                    RowLayout {
                        visible: !root.ready || root.st.count === null || root.st.count === undefined
                        spacing: Theme.spacing.small
                        LogosSpinner {
                            running: visible
                            Layout.preferredWidth: 14
                            Layout.preferredHeight: 14
                            thickness: 2
                            dotSize: 4
                        }
                        Caption {
                            text: "Loading therapists…"
                        }
                    }

                    // empty
                    Card {
                        visible: root.ready && root.st.count !== null && root.st.count !== undefined && root.directory.length === 0
                        Title {
                            text: root.search.trim().length || root.verifiedOnly ? "No therapists match that" : "No therapists here yet"
                        }
                        Caption {
                            text: root.search.trim().length || root.verifiedOnly ? "Try a different word, or turn off “Verified only”." : "Ghostmind is new on this network. Check back soon."
                        }
                    }

                    Repeater {
                        model: root.directory
                        delegate: TherapistCard {
                            required property var modelData
                            t: modelData
                        }
                    }

                    // how it works
                    Card {
                        pad: Theme.spacing.large
                        RowLayout {
                            Layout.fillWidth: true
                            Title {
                                text: "How Ghostmind keeps you safe"
                                font.pixelSize: Theme.typography.primaryText
                            }
                            Link {
                                text: root.showHowItWorks ? "Hide" : "Show"
                                onClicked: root.showHowItWorks = !root.showHowItWorks
                            }
                        }
                        ColumnLayout {
                            visible: root.showHowItWorks
                            Layout.fillWidth: true
                            spacing: Theme.spacing.medium
                            Repeater {
                                model: [
                                    { t: "Anonymous by design", d: "You don't create an account. Each conversation uses a fresh, one-time key, so the therapist sees only a nickname like “Client 3fa2”." },
                                    { t: "Encrypted messages", d: "Messages travel over Logos messaging, a peer-to-peer network. They're encrypted on your computer and can only be read by the therapist." },
                                    { t: "Private payments on LEZ", d: "You pay from your private account on the Logos Execution Zone. If it's empty, Ghostmind shields LGO into it for you first. The payment itself doesn't show a sender, a recipient or an amount." },
                                    { t: "Verified licences", d: "Licensing boards appointed by Ghostmind's council check each therapist's licence and record the result publicly. The licence number itself is never published." },
                                    { t: "Honest, anonymous reviews", d: "Only clients who paid can leave a review, and reviews are posted from a separate account that can't be linked to you. Three reports from paying clients hide a therapist until the council looks into it." }
                                ]
                                delegate: ColumnLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: 2
                                    LogosText {
                                        text: modelData.t
                                        color: Theme.palette.text
                                        font.pixelSize: Theme.typography.secondaryText
                                        font.weight: Theme.typography.weightBold
                                    }
                                    Caption {
                                        text: modelData.d
                                    }
                                }
                            }
                            Caption {
                                text: "Worth knowing: the people relaying messages can see your internet address (not what you say). Use a VPN or Tor if that matters to you."
                                color: Theme.palette.textSecondary
                            }
                        }
                    }
                    CrisisNote {}
                }

                // ════ Messages ════
                ColumnLayout {
                    visible: root.page === "messages" && !root.showWelcome
                    Layout.fillWidth: true
                    spacing: Theme.spacing.large

                    // list
                    ColumnLayout {
                        visible: !root.current
                        Layout.fillWidth: true
                        spacing: Theme.spacing.small

                        Card {
                            visible: root.conversations.length === 0
                            Title {
                                text: root.pro ? "No clients yet" : "No messages yet"
                            }
                            Caption {
                                text: root.pro ? (root.mine ? "When a client writes to you, the conversation shows up here. Clients appear only as a nickname." : "Publish your listing under My practice so clients can find you.") : "Find a therapist and send them a message. They'll only see a nickname, never who you are."
                            }
                            Pill {
                                visible: !root.pro || !root.mine
                                text: root.pro ? "Go to My practice" : "Find a therapist"
                                onClicked: root.tab = 0
                            }
                        }

                        Repeater {
                            model: root.conversations
                            delegate: Rectangle {
                                id: row
                                required property var modelData
                                readonly property var last: modelData.messages && modelData.messages.length ? modelData.messages[modelData.messages.length - 1] : null
                                Layout.fillWidth: true
                                implicitHeight: rowCol.implicitHeight + 2 * Theme.spacing.large
                                radius: Theme.spacing.radiusXlarge
                                color: rowHover.hovered ? Theme.palette.backgroundSecondary : Theme.palette.backgroundElevated
                                border.color: modelData.unread > 0 ? Theme.colors.getColor(Theme.palette.primary, 0.6) : Theme.palette.borderSecondary
                                HoverHandler {
                                    id: rowHover
                                }
                                RowLayout {
                                    id: rowCol
                                    anchors.fill: parent
                                    anchors.margins: Theme.spacing.large
                                    spacing: Theme.spacing.medium
                                    Avatar {
                                        seed: row.modelData.role === "client" ? row.modelData.peerAccount : row.modelData.id
                                        name: row.modelData.peerName
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        RowLayout {
                                            spacing: Theme.spacing.small
                                            LogosText {
                                                text: row.modelData.peerName
                                                color: Theme.palette.text
                                                font.pixelSize: Theme.typography.primaryText
                                                font.weight: Theme.typography.weightBold
                                            }
                                            Chip {
                                                visible: row.modelData.purpose === "verification"
                                                text: "Licence check"
                                                tint: Theme.palette.info
                                            }
                                            Chip {
                                                visible: row.modelData.paid === true
                                                text: row.modelData.rated ? "Reviewed" : "Paid"
                                                tint: Theme.palette.success
                                            }
                                        }
                                        Caption {
                                            text: row.last ? (row.last.from === "me" ? "You: " : "") + row.last.text : "No messages yet"
                                            color: row.modelData.unread > 0 ? Theme.palette.text : Theme.palette.textTertiary
                                            elide: Text.ElideRight
                                            maximumLineCount: 1
                                            wrapMode: Text.NoWrap
                                        }
                                    }
                                    ColumnLayout {
                                        spacing: 4
                                        LogosText {
                                            Layout.alignment: Qt.AlignRight
                                            text: root.whenOf(row.modelData.lastMs)
                                            color: Theme.palette.textTertiary
                                            font.pixelSize: Theme.typography.secondaryText
                                        }
                                        Rectangle {
                                            Layout.alignment: Qt.AlignRight
                                            visible: row.modelData.unread > 0
                                            implicitWidth: Math.max(20, unreadLbl.implicitWidth + 10)
                                            implicitHeight: 20
                                            radius: 10
                                            color: Theme.palette.primary
                                            LogosText {
                                                id: unreadLbl
                                                anchors.centerIn: parent
                                                text: row.modelData.unread
                                                color: "#ffffff"
                                                font.pixelSize: 11
                                                font.weight: Theme.typography.weightBold
                                            }
                                        }
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: root.openConv(row.modelData.id)
                                }
                            }
                        }
                    }

                    // one conversation
                    ColumnLayout {
                        id: chat
                        visible: !!root.current
                        Layout.fillWidth: true
                        spacing: Theme.spacing.medium
                        readonly property var c: root.current || ({ id: "", peerName: "", peerAccount: "", role: "", purpose: "", messages: [], paid: false, ticketIssued: false, rated: false, acked: false, licence: "", jurisdiction: "", expires: "", paidAmount: "" })
                        readonly property var t: c.therapist || root.therapistOf(c.peerAccount)
                        readonly property bool clientTherapy: c.role === "client" && c.purpose === "therapy"
                        readonly property bool hostTherapy: c.role === "host" && c.purpose === "therapy"
                        readonly property bool hostVerify: c.role === "host" && c.purpose === "verification"
                        readonly property bool clientVerify: c.role === "client" && c.purpose === "verification"
                        readonly property string nickname: "Client " + (c.id || "").substring(0, 4)
                        readonly property bool saidHello: (c.messages || []).some(function (m) { return m.from === "me" && m.kind === "msg"; })
                        readonly property var busyJob: root.activeJobs.filter(function (j) { return j.conversation === c.id; })[0] || null
                        readonly property bool busy: !!c.busy || !!busyJob
                        readonly property string busyLine: c.busy ? root.jobText(c.busy) : busyJob ? root.jobText(busyJob) : ""

                        // top bar
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.medium
                            Link {
                                text: "‹  Messages"
                                font.pixelSize: Theme.typography.primaryText
                                onClicked: {
                                    root.selectedConv = "";
                                    root.confirmDelete = false;
                                }
                            }
                            Item {
                                Layout.fillWidth: true
                            }
                            Link {
                                text: root.confirmDelete ? "Delete it from this computer?" : "Delete conversation"
                                color: root.confirmDelete ? Theme.palette.error : Theme.palette.textTertiary
                                font.weight: Theme.typography.weightRegular
                                onClicked: {
                                    if (!root.confirmDelete) {
                                        root.confirmDelete = true;
                                        return;
                                    }
                                    var id = chat.c.id;
                                    root.confirmDelete = false;
                                    root.selectedConv = "";
                                    root.callArgs("closeConversation", [id]);
                                }
                            }
                            Link {
                                visible: root.confirmDelete
                                text: "Keep it"
                                onClicked: root.confirmDelete = false
                            }
                        }

                        // who you're talking to
                        Card {
                            pad: Theme.spacing.large
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing.medium
                                Avatar {
                                    seed: chat.c.role === "client" ? chat.c.peerAccount : chat.c.id
                                    name: chat.c.peerName
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    RowLayout {
                                        spacing: Theme.spacing.small
                                        LogosText {
                                            text: chat.c.peerName || ""
                                            color: Theme.palette.text
                                            font.pixelSize: Theme.typography.subtitleText
                                            font.weight: Theme.typography.weightBold
                                        }
                                        Chip {
                                            visible: chat.clientTherapy && !!chat.t && chat.t.verified === true
                                            text: "✓ Verified"
                                            tint: Theme.palette.success
                                        }
                                    }
                                    Caption {
                                        text: chat.clientTherapy ? (chat.t ? root.ratingLine(chat.t) + " · " + chat.t.price + " LGO per session" : "Therapist") : chat.hostTherapy ? "Anonymous client. You can't see who they are, and neither can anyone else." : chat.hostVerify ? "A therapist asking you to check their licence" : "Licensing body checking your licence"
                                    }
                                }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 1
                                color: Theme.palette.borderSecondary
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing.small
                                LogosText {
                                    text: "🔒"
                                    font.pixelSize: Theme.typography.secondaryText
                                }
                                Caption {
                                    text: chat.clientTherapy ? "End-to-end encrypted. " + (chat.c.peerName || "The therapist") + " sees you as “" + chat.nickname + "”." : "End-to-end encrypted."
                                }
                            }

                            // progress through a session (client)
                            RowLayout {
                                visible: chat.clientTherapy
                                Layout.fillWidth: true
                                Layout.topMargin: 4
                                spacing: Theme.spacing.small
                                Step {
                                    n: 1
                                    text: "Say hello"
                                    done: chat.saidHello || chat.c.paid
                                    active: !done
                                }
                                StepLine {}
                                Step {
                                    n: 2
                                    text: "Pay privately"
                                    done: chat.c.paid
                                    active: !chat.c.paid && (chat.saidHello || chat.busy)
                                }
                                StepLine {}
                                Step {
                                    n: 3
                                    text: "Review (optional)"
                                    done: chat.c.rated
                                    active: chat.c.paid && !chat.c.rated
                                }
                            }
                        }

                        // messages
                        Caption {
                            visible: (chat.c.messages || []).length === 0
                            Layout.topMargin: Theme.spacing.medium
                            horizontalAlignment: Text.AlignHCenter
                            text: chat.clientTherapy ? "Say hello and tell " + (chat.c.peerName || "them") + " a little about what's on your mind. Share only what you're comfortable with." : ""
                        }
                        Repeater {
                            model: chat.c.messages || []
                            delegate: Item {
                                id: msg
                                required property var modelData
                                readonly property bool mineMsg: modelData.from === "me"
                                readonly property bool system: modelData.kind !== "msg"
                                Layout.fillWidth: true
                                implicitHeight: msg.system ? sysText.implicitHeight + 8 : bubble.implicitHeight
                                LogosText {
                                    id: sysText
                                    visible: msg.system
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    anchors.verticalCenter: parent.verticalCenter
                                    width: Math.min(parent.width * 0.8, implicitWidth)
                                    horizontalAlignment: Text.AlignHCenter
                                    wrapMode: Text.Wrap
                                    textFormat: Text.PlainText
                                    text: msg.modelData.text + "  ·  " + root.timeOf(msg.modelData.time)
                                    color: Theme.palette.textTertiary
                                    font.pixelSize: Theme.typography.secondaryText
                                }
                                Rectangle {
                                    id: bubble
                                    visible: !msg.system
                                    width: Math.min(parent.width * 0.75, bubbleText.implicitWidth + 2 * Theme.spacing.large)
                                    implicitHeight: bubbleCol.implicitHeight + 2 * Theme.spacing.medium
                                    radius: 16
                                    color: msg.mineMsg ? Theme.colors.getColor(Theme.palette.primary, 0.85) : Theme.palette.backgroundElevated
                                    border.color: msg.mineMsg ? "transparent" : Theme.palette.borderSecondary
                                    anchors.right: msg.mineMsg ? parent.right : undefined
                                    ColumnLayout {
                                        id: bubbleCol
                                        anchors.fill: parent
                                        anchors.leftMargin: Theme.spacing.large
                                        anchors.rightMargin: Theme.spacing.large
                                        anchors.topMargin: Theme.spacing.medium
                                        anchors.bottomMargin: Theme.spacing.medium
                                        spacing: 2
                                        LogosText {
                                            id: bubbleText
                                            Layout.fillWidth: true
                                            text: msg.modelData.text
                                            wrapMode: Text.Wrap
                                            textFormat: Text.PlainText
                                            color: msg.mineMsg ? "#ffffff" : Theme.palette.text
                                            font.pixelSize: Theme.typography.primaryText
                                            lineHeight: 1.2
                                        }
                                        LogosText {
                                            Layout.alignment: Qt.AlignRight
                                            text: root.timeOf(msg.modelData.time)
                                            color: msg.mineMsg ? Qt.rgba(1, 1, 1, 0.7) : Theme.palette.textMuted
                                            font.pixelSize: 11
                                        }
                                    }
                                }
                            }
                        }
                        RowLayout {
                            visible: !chat.c.acked && (chat.c.messages || []).length > 0
                            Layout.alignment: Qt.AlignRight
                            spacing: 6
                            LogosSpinner {
                                running: visible
                                Layout.preferredWidth: 12
                                Layout.preferredHeight: 12
                                thickness: 2
                                dotSize: 3
                            }
                            Caption {
                                Layout.fillWidth: false
                                text: "Delivering. " + (chat.c.peerName || "They") + " will get it as soon as they're online."
                            }
                        }

                        // composer
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.topMargin: Theme.spacing.small
                            spacing: Theme.spacing.small
                            Field {
                                id: composer
                                implicitHeight: 46
                                placeholderText: "Write a message…"
                            }
                            Pill {
                                text: "Send"
                                implicitHeight: 46
                                radius: 23
                                can: root.ready && composer.text.trim().length > 0
                                onClicked: root.sendCurrent()
                            }
                        }
                        Connections {
                            target: composer.textInput
                            function onAccepted() {
                                root.sendCurrent();
                            }
                        }

                        // ── client: pay, then review ──
                        Card {
                            visible: chat.clientTherapy
                            Layout.topMargin: Theme.spacing.small

                            // not paid
                            ColumnLayout {
                                visible: !chat.c.paid
                                Layout.fillWidth: true
                                spacing: Theme.spacing.medium
                                RowLayout {
                                    Layout.fillWidth: true
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2
                                        Title {
                                            text: chat.busy ? "Paying privately…" : "Book a session"
                                        }
                                        Caption {
                                            text: chat.busy ? "This takes a few minutes. You can keep chatting in the meantime." : "Agree on a time in the chat, then pay here. " + (chat.c.peerName || "The therapist") + " is told the moment it arrives."
                                        }
                                    }
                                    LogosText {
                                        visible: !!chat.t
                                        text: chat.t ? chat.t.price + " LGO" : ""
                                        color: Theme.palette.text
                                        font.pixelSize: Theme.typography.panelTitleText
                                        font.weight: Theme.typography.weightBold
                                    }
                                }
                                Notice {
                                    visible: chat.busy
                                    tint: Theme.palette.primary
                                    LogosSpinner {
                                        running: visible
                                        Layout.preferredWidth: 14
                                        Layout.preferredHeight: 14
                                        thickness: 2
                                        dotSize: 4
                                    }
                                    Caption {
                                        color: Theme.palette.text
                                        text: chat.busyLine
                                    }
                                }
                                Pill {
                                    visible: !chat.busy
                                    text: chat.t ? "Pay " + chat.t.price + " LGO privately" : "Pay privately"
                                    can: root.ready && !!chat.t && chat.t.status === 0
                                    onClicked: root.callArgs("pay", [chat.c.id])
                                }
                                Caption {
                                    text: "Paid from your private account on the Logos Execution Zone (LEZ), so nobody can see who you paid or how much. If that account is short, Ghostmind shields LGO into it first (and on the testnet, tops you up from the faucet)."
                                }
                            }

                            // paid, waiting for the therapist's confirmation
                            ColumnLayout {
                                visible: chat.c.paid && !chat.c.ticketIssued
                                Layout.fillWidth: true
                                spacing: Theme.spacing.small
                                Title {
                                    text: "✓ Paid " + chat.c.paidAmount + " LGO"
                                    color: Theme.palette.success
                                }
                                Caption {
                                    text: (chat.c.peerName || "The therapist") + " is confirming your payment. Once that's done, usually within a few minutes, you'll be able to leave an anonymous review."
                                }
                            }

                            // review
                            ColumnLayout {
                                visible: chat.c.ticketIssued && !chat.c.rated
                                Layout.fillWidth: true
                                spacing: Theme.spacing.medium
                                ColumnLayout {
                                    spacing: 2
                                    Layout.fillWidth: true
                                    Title {
                                        text: "How was your session?"
                                    }
                                    Caption {
                                        text: "Your review is posted from a separate account, so it can't be traced to you or to this chat. Take your time. There's no deadline."
                                    }
                                }
                                RowLayout {
                                    spacing: 6
                                    Repeater {
                                        model: 5
                                        delegate: LogosText {
                                            required property int index
                                            text: index < root.ratingScore ? "★" : "☆"
                                            color: index < root.ratingScore ? "#F5B343" : Theme.palette.textTertiary
                                            font.pixelSize: 32
                                            MouseArea {
                                                anchors.fill: parent
                                                cursorShape: Qt.PointingHandCursor
                                                onClicked: root.ratingScore = index + 1
                                            }
                                        }
                                    }
                                    Caption {
                                        Layout.fillWidth: false
                                        Layout.leftMargin: Theme.spacing.small
                                        text: ["Tap a star", "Poor", "Not great", "Okay", "Good", "Excellent"][root.ratingScore]
                                    }
                                }
                                Field {
                                    id: reviewText
                                    placeholderText: "A few words for others (optional, shown publicly)"
                                }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: Theme.spacing.small
                                    LogosCheckbox {
                                        id: reportBox
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        LogosText {
                                            text: "Report a problem with this therapist"
                                            color: Theme.palette.text
                                            font.pixelSize: Theme.typography.secondaryText
                                            MouseArea {
                                                anchors.fill: parent
                                                onClicked: reportBox.checked = !reportBox.checked
                                            }
                                        }
                                        Caption {
                                            text: "For unprofessional or unsafe behaviour. After three reports a therapist is hidden while Ghostmind's council reviews them."
                                        }
                                    }
                                }
                                Pill {
                                    text: chat.busy ? "Posting…" : reportBox.checked ? "Post review and report" : "Post anonymous review"
                                    can: root.ready && root.ratingScore > 0 && !chat.busy
                                    busy: chat.busy
                                    danger: reportBox.checked
                                    onClicked: root.callArgs("rate", [chat.c.id, root.ratingScore, reviewText.text, reportBox.checked], function (r) {
                                        if (r && r.ok)
                                            reviewText.text = "";
                                    })
                                }
                            }

                            // reviewed
                            ColumnLayout {
                                visible: chat.c.rated
                                Layout.fillWidth: true
                                spacing: Theme.spacing.small
                                Title {
                                    text: "✓ Thanks for your review"
                                    color: Theme.palette.success
                                }
                                Caption {
                                    text: "It's posted anonymously and helps other people find good care. You can keep talking here as long as you like."
                                }
                            }
                        }

                        // ── therapist: client's payment ──
                        Card {
                            visible: chat.hostTherapy
                            Layout.topMargin: Theme.spacing.small
                            Title {
                                text: chat.c.ticketIssued ? "✓ Paid " + chat.c.paidAmount + " LGO" : chat.c.paid ? "Payment received · confirming" : "Not paid yet"
                                color: chat.c.ticketIssued ? Theme.palette.success : Theme.palette.text
                            }
                            Caption {
                                text: chat.c.ticketIssued ? "The payment is confirmed and this client can now leave a review. It reached your private account, so nobody else can see it." : chat.c.paid ? "Ghostmind is checking that " + chat.c.paidAmount + " LGO reached your private account. When it has, the client is allowed to review you (Ghostmind's fee for that is " + root.feeText + ")." : "When this client pays, it lands in your private account and Ghostmind confirms it for you automatically."
                            }
                            Notice {
                                visible: chat.busy
                                tint: Theme.palette.primary
                                LogosSpinner {
                                    running: visible
                                    Layout.preferredWidth: 14
                                    Layout.preferredHeight: 14
                                    thickness: 2
                                    dotSize: 4
                                }
                                Caption {
                                    color: Theme.palette.text
                                    text: chat.busyLine
                                }
                            }
                            Pill {
                                visible: chat.c.paid && !chat.c.ticketIssued && !chat.busy
                                text: "Confirm payment now"
                                secondary: true
                                can: root.ready
                                onClicked: root.callArgs("issueTicket", [chat.c.id])
                            }
                        }

                        // ── verifier: a licence to check ──
                        Card {
                            visible: chat.hostVerify
                            Layout.topMargin: Theme.spacing.small
                            Title {
                                text: chat.t && chat.t.verified ? "✓ You verified this licence" : "Licence to check"
                            }
                            GridLayout {
                                Layout.fillWidth: true
                                columns: 2
                                columnSpacing: Theme.spacing.large
                                rowSpacing: 4
                                Caption {
                                    Layout.fillWidth: false
                                    text: "Licence number"
                                }
                                Body {
                                    text: chat.c.licence || "Not received yet"
                                    color: Theme.palette.text
                                }
                                Caption {
                                    Layout.fillWidth: false
                                    text: "Jurisdiction"
                                }
                                Body {
                                    text: chat.c.jurisdiction || "—"
                                    color: Theme.palette.text
                                }
                                Caption {
                                    Layout.fillWidth: false
                                    text: "Expires"
                                }
                                Body {
                                    text: chat.c.expires || "Not given"
                                    color: Theme.palette.text
                                }
                            }
                            CopyRow {
                                label: "Therapist's LEZ account"
                                value: chat.c.peerAccount || ""
                            }
                            Caption {
                                text: "Check this with the issuing body before you confirm. Confirming publishes your approval with the jurisdiction and expiry date. The licence number itself stays private."
                            }
                            Notice {
                                visible: chat.busy
                                tint: Theme.palette.primary
                                LogosSpinner {
                                    running: visible
                                    Layout.preferredWidth: 14
                                    Layout.preferredHeight: 14
                                    thickness: 2
                                    dotSize: 4
                                }
                                Caption {
                                    color: Theme.palette.text
                                    text: chat.busyLine
                                }
                            }
                            RowLayout {
                                spacing: Theme.spacing.medium
                                Pill {
                                    visible: !(chat.t && chat.t.verified)
                                    text: "Confirm licence is valid"
                                    can: root.ready && (chat.c.licence || "").length > 0 && !chat.busy
                                    onClicked: root.callArgs("verifyTherapist", [chat.c.peerAccount, chat.c.licence, chat.c.jurisdiction, chat.c.expires])
                                }
                                Pill {
                                    visible: !!chat.t && chat.t.verified === true
                                    text: "Withdraw verification"
                                    danger: true
                                    can: root.ready && !chat.busy
                                    onClicked: root.callArgs("revokeTherapist", [chat.c.peerAccount])
                                }
                            }
                        }

                        // ── therapist: waiting on a verifier ──
                        Card {
                            visible: chat.clientVerify
                            Layout.topMargin: Theme.spacing.small
                            Title {
                                text: root.mine && root.mine.verified ? "✓ Your licence is verified" : "Waiting for the check"
                                color: root.mine && root.mine.verified ? Theme.palette.success : Theme.palette.text
                            }
                            Caption {
                                text: root.mine && root.mine.verified ? "Clients now see a Verified badge on your listing." : (chat.c.peerName || "The licensing body") + " has your details, sent encrypted. They'll check them with the issuing body and you'll get a note here when they're done."
                            }
                        }
                    }
                }

                // ════ Wallet ════
                ColumnLayout {
                    visible: root.page === "wallet" && !root.showWelcome
                    Layout.fillWidth: true
                    spacing: Theme.spacing.large

                    Notice {
                        tint: Theme.palette.info
                        Caption {
                            color: Theme.palette.textSecondary
                            text: root.pro ? "Client payments arrive in your private account. Ghostmind's small fee per paid session comes from your public account, which is topped up from the testnet faucet automatically. LGO here is free test money." : "You don't need to do anything here to use Ghostmind. Paying a therapist tops up and shields LGO for you automatically. This is the Logos testnet, so LGO here is free test money."
                        }
                    }

                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Theme.spacing.large
                        Card {
                            Layout.preferredWidth: 1
                            LogosText {
                                text: "Private account"
                                color: Theme.palette.textSecondary
                                font.pixelSize: Theme.typography.secondaryText
                                font.weight: Theme.typography.weightBold
                            }
                            LogosText {
                                text: (root.me.lgoPrivate || "0") + " LGO"
                                color: Theme.palette.text
                                font.pixelSize: Theme.typography.titleText
                                font.weight: Theme.typography.weightBold
                            }
                            Caption {
                                text: root.pro ? "Your hidden balance on the Logos Execution Zone (LEZ). Clients pay you here, and nobody else can see who paid or how much." : "Your hidden balance on the Logos Execution Zone (LEZ). Therapy is paid from here, and nobody else can see it or where it goes."
                            }
                        }
                        Card {
                            Layout.preferredWidth: 1
                            LogosText {
                                text: "Public account"
                                color: Theme.palette.textSecondary
                                font.pixelSize: Theme.typography.secondaryText
                                font.weight: Theme.typography.weightBold
                            }
                            LogosText {
                                text: (root.me.lgo || "0") + " LGO"
                                color: Theme.palette.text
                                font.pixelSize: Theme.typography.titleText
                                font.weight: Theme.typography.weightBold
                            }
                            Caption {
                                text: "Visible to everyone. It covers small network fees. Shield LGO to move it into your private account."
                            }
                        }
                    }

                    Card {
                        Title {
                            text: "Add funds"
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.medium
                            Pill {
                                text: "Get test LGO from the faucet"
                                secondary: true
                                can: root.ready
                                busy: root.activeJobs.some(function (j) { return j.kind === "gas"; })
                                onClicked: root.callArgs("getGas", [])
                            }
                            Caption {
                                text: "Free LGO for the testnet, added to your public account."
                            }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            implicitHeight: 1
                            color: Theme.palette.borderSecondary
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Theme.spacing.medium
                            Field {
                                id: shieldAmt
                                Layout.preferredWidth: 140
                                Layout.fillWidth: false
                                placeholderText: "Amount in LGO"
                            }
                            Pill {
                                text: "Shield to private"
                                secondary: true
                                can: root.ready && shieldAmt.text.trim().length > 0
                                busy: root.activeJobs.some(function (j) { return j.kind === "shield"; })
                                onClicked: root.callArgs("shield", [shieldAmt.text], function (r) {
                                    if (r && r.ok)
                                        shieldAmt.text = "";
                                })
                            }
                            Caption {
                                text: "Moves LGO from public to private. Doing this ahead of time makes your payments even harder to connect to you."
                            }
                        }
                    }

                    Card {
                        RowLayout {
                            Layout.fillWidth: true
                            IconBubble {
                                glyph: "!"
                                glyphColor: Theme.palette.warning
                                color: Theme.colors.getColor(Theme.palette.warning, 0.16)
                                Layout.alignment: Qt.AlignTop
                            }
                            ColumnLayout {
                                Layout.fillWidth: true
                                spacing: 2
                                Title {
                                    text: "Recovery phrase"
                                }
                                Caption {
                                    text: root.pro ? "24 words that are the only way to get this wallet back if this computer is lost: your LGO and your listing. Write them down and keep them somewhere safe. Never share them with anyone." : "24 words that are the only way to get this wallet back if this computer is lost. Write them down and keep them somewhere safe. Never share them with anyone."
                                }
                            }
                        }
                        Pill {
                            text: root.showRecovery ? "Hide recovery phrase" : "Show recovery phrase"
                            secondary: true
                            onClicked: {
                                if (!root.showRecovery)
                                    root.callArgs("recoveryPhrase", [], function (r) {
                                        if (r && r.ok)
                                            root.recoveryWords = r.phrase;
                                    });
                                root.showRecovery = !root.showRecovery;
                            }
                        }
                        Flow {
                            visible: root.showRecovery && root.recoveryWords.length > 0
                            Layout.fillWidth: true
                            spacing: 6
                            Repeater {
                                model: root.recoveryWords.split(/\s+/).filter(function (w) { return w.length; })
                                delegate: Rectangle {
                                    required property string modelData
                                    required property int index
                                    implicitWidth: wordLbl.implicitWidth + 16
                                    implicitHeight: 28
                                    radius: 6
                                    color: Theme.palette.backgroundSecondary
                                    LogosText {
                                        id: wordLbl
                                        anchors.centerIn: parent
                                        text: (index + 1) + ". " + modelData
                                        font.family: "Menlo"
                                        color: Theme.palette.text
                                        font.pixelSize: Theme.typography.secondaryText
                                    }
                                }
                            }
                        }
                    }

                    // advanced
                    Card {
                        pad: Theme.spacing.large
                        RowLayout {
                            Layout.fillWidth: true
                            Title {
                                text: "Advanced"
                                font.pixelSize: Theme.typography.primaryText
                            }
                            Link {
                                text: root.showAdvanced ? "Hide" : "Addresses, network, activity and settings"
                                onClicked: root.showAdvanced = !root.showAdvanced
                            }
                        }
                        ColumnLayout {
                            visible: root.showAdvanced
                            Layout.fillWidth: true
                            spacing: Theme.spacing.medium
                            CopyRow {
                                label: "Public account address"
                                value: root.me.address || ""
                            }
                            CopyRow {
                                label: "Private account (receiving address)"
                                value: root.me.privateAddress || ""
                                hint: "Synced up to block " + (root.net.synced || 0)
                            }
                            CopyRow {
                                label: "Chat key"
                                value: root.me.chatKey || ""
                                hint: "Messages to you are encrypted to this key. A licensing body shares it with Ghostmind's council when it's appointed."
                            }
                            CopyRow {
                                label: "Network"
                                value: "LEZ testnet " + (root.net.online ? "online · block " + root.net.block : "offline") + " · Logos messaging " + (root.net.messaging === 2 ? "connected" : root.net.messaging === 3 ? "down" : "connecting")
                            }
                            CopyRow {
                                label: "Ghostmind registry program"
                                value: root.program.address || ""
                            }
                            CopyRow {
                                label: "Council"
                                value: root.program.council || ""
                            }
                            CopyRow {
                                label: "Treasury (Ghostmind fees) · " + (root.program.treasuryLgo || "0") + " LGO"
                                value: root.program.treasury || ""
                            }
                            RowLayout {
                                visible: !root.modeLocked
                                Layout.fillWidth: true
                                spacing: Theme.spacing.medium
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 1
                                    FieldLabel {
                                        text: "How you use Ghostmind"
                                    }
                                    Caption {
                                        text: root.pro ? "Therapist mode. Switch if you came here to find help." : "Looking for help. Switch if you're a therapist offering sessions."
                                    }
                                }
                                Pill {
                                    text: root.pro ? "Switch to client mode" : "Switch to therapist mode"
                                    secondary: true
                                    onClicked: root.chooseMode(root.pro ? "client" : "therapist")
                                }
                            }
                            FieldLabel {
                                text: "Use a different registry"
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing.small
                                Field {
                                    id: programField
                                    placeholderText: "Program address (leave empty for the built-in one)"
                                    text: root.program.custom ? root.program.address : ""
                                }
                                Pill {
                                    text: "Use"
                                    secondary: true
                                    can: root.ready
                                    onClicked: root.callArgs("setProgram", [programField.text.trim()])
                                }
                            }
                            FieldLabel {
                                visible: root.jobs.length > 0
                                text: "Recent activity"
                            }
                            Repeater {
                                model: root.jobs.slice(0, 12)
                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: Theme.spacing.medium
                                    Chip {
                                        text: modelData.status === "done" ? "Done" : modelData.status === "failed" ? "Failed" : "Working"
                                        tint: modelData.status === "done" ? Theme.palette.success : modelData.status === "failed" ? Theme.palette.error : Theme.palette.warning
                                    }
                                    Caption {
                                        text: root.jobVerb(modelData) + (modelData.error ? " · " + root.friendlyError(modelData.error) : "") + (modelData.tx ? " · tx " + modelData.tx.substring(0, 10) + "…" : "")
                                        color: Theme.palette.text
                                    }
                                    Caption {
                                        Layout.fillWidth: false
                                        text: root.whenOf(modelData.created)
                                    }
                                }
                            }
                        }
                    }
                }

                // ════ Therapist pages ════
                ColumnLayout {
                    visible: root.pro && !root.showWelcome && (root.page === "practice" || root.page === "checks" || root.page === "council")
                    Layout.fillWidth: true
                    spacing: Theme.spacing.large

                    // ── Your practice ──
                    ColumnLayout {
                        visible: root.page === "practice"
                        Layout.fillWidth: true
                        spacing: Theme.spacing.large

                        // intro for new therapists
                        Card {
                            visible: !root.mine
                            Heading {
                                text: "Offer sessions on Ghostmind"
                            }
                            Body {
                                text: "Meet clients who need privacy: people who can't risk an employer, family or insurer finding out they're in therapy. You get paid privately in LGO, and your reputation comes only from clients who actually paid."
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                Layout.topMargin: Theme.spacing.small
                                spacing: Theme.spacing.small
                                Step {
                                    n: 1
                                    text: "Create your listing"
                                    active: true
                                }
                                StepLine {}
                                Step {
                                    n: 2
                                    text: "Get your licence checked"
                                }
                                StepLine {}
                                Step {
                                    n: 3
                                    text: "Talk to clients, get paid"
                                }
                            }
                        }

                        // listing form
                        Card {
                            visible: !root.mine || root.editingProfile
                            Title {
                                text: root.mine ? "Edit your listing" : "Your listing"
                            }
                            FieldLabel {
                                text: "Name clients will see"
                            }
                            Field {
                                id: nameField
                                placeholderText: "e.g. Dr. Ana Ruiz"
                            }
                            FieldLabel {
                                text: "What you help with"
                            }
                            Field {
                                id: specField
                                placeholderText: "Separate with commas, e.g. anxiety, grief, burnout"
                            }
                            FieldLabel {
                                text: "About you"
                            }
                            LogosTextArea {
                                id: bioField
                                Layout.fillWidth: true
                                Layout.preferredHeight: 110
                                placeholderText: "Your approach, experience and languages. Two or three sentences is plenty."
                                wrapMode: TextEdit.Wrap
                            }
                            FieldLabel {
                                text: "Price per session"
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing.medium
                                Field {
                                    id: priceField
                                    Layout.preferredWidth: 140
                                    Layout.fillWidth: false
                                    placeholderText: "e.g. 0.5"
                                }
                                LogosText {
                                    text: "LGO"
                                    color: Theme.palette.textSecondary
                                    font.pixelSize: Theme.typography.primaryText
                                }
                                Caption {
                                    text: "Ghostmind keeps 2% of each paid session (at least 0.01 LGO). That small fee is what stops fake reviews."
                                }
                            }
                            Caption {
                                text: "Your listing is public on the Logos Execution Zone (LEZ). It includes a private receiving address so clients can pay you without either of you being identified."
                            }
                            RowLayout {
                                spacing: Theme.spacing.medium
                                Pill {
                                    text: root.mine ? "Save changes" : "Publish my listing"
                                    can: root.ready && nameField.text.trim().length > 0 && priceField.text.trim().length > 0
                                    busy: root.activeJobs.some(function (j) { return j.kind === "register" || j.kind === "update"; })
                                    onClicked: {
                                        root.lastError = "";
                                        root.callArgs(root.mine ? "updateProfile" : "registerTherapist", [nameField.text, bioField.text, specField.text, priceField.text], function (r) {
                                            if (r && r.ok)
                                                root.editingProfile = false;
                                        });
                                    }
                                }
                                Link {
                                    visible: root.mine !== null
                                    text: "Cancel"
                                    onClicked: root.editingProfile = false
                                }
                            }
                        }

                        // existing listing
                        ColumnLayout {
                            visible: root.mine !== null && !root.editingProfile
                            Layout.fillWidth: true
                            spacing: Theme.spacing.large

                            Notice {
                                visible: !!root.mine && root.mine.status !== 0
                                tint: Theme.palette.error
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2
                                    Title {
                                        text: root.mine && root.mine.status === 1 ? "Your listing is suspended" : "Your listing is hidden after client reports"
                                        font.pixelSize: Theme.typography.primaryText
                                    }
                                    Caption {
                                        color: Theme.palette.textSecondary
                                        text: ((root.mine && root.mine.reason) ? "Reason: " + root.mine.reason + ". " : "") + "Clients can't find you until Ghostmind's council reinstates it."
                                    }
                                }
                            }

                            // at a glance
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing.large
                                Repeater {
                                    model: [
                                        { k: "Paid sessions", v: root.mine ? String(root.mine.tickets || 0) : "0" },
                                        { k: "Rating", v: root.mine && root.mine.ratingCount > 0 ? Number(root.mine.rating).toFixed(1) + " ★" : "—" },
                                        { k: "Reviews", v: root.mine ? String(root.mine.ratingCount || 0) : "0" },
                                        { k: "Private balance", v: (root.me.lgoPrivate || "0") + " LGO" }
                                    ]
                                    delegate: Card {
                                        required property var modelData
                                        Layout.preferredWidth: 1
                                        pad: Theme.spacing.large
                                        LogosText {
                                            text: modelData.k
                                            color: Theme.palette.textTertiary
                                            font.pixelSize: Theme.typography.secondaryText
                                        }
                                        LogosText {
                                            text: modelData.v
                                            color: Theme.palette.text
                                            font.pixelSize: Theme.typography.panelTitleText
                                            font.weight: Theme.typography.weightBold
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                FieldLabel {
                                    text: "How clients see you"
                                }
                                Item {
                                    Layout.fillWidth: true
                                }
                                Link {
                                    text: "Edit listing"
                                    onClicked: {
                                        nameField.text = root.mine.name;
                                        specField.text = root.mine.specialties;
                                        bioField.text = root.mine.bio;
                                        priceField.text = root.mine.price;
                                        root.editingProfile = true;
                                    }
                                }
                            }
                            TherapistCard {
                                preview: true
                                t: root.mine || ({ account: "", name: "", specialties: "", bio: "", price: "", status: 0, rating: 0, ratingCount: 0, reports: 0, tickets: 0, reviews: 0 })
                            }

                            // verification
                            Card {
                                Title {
                                    text: root.mine && root.mine.verified ? "✓ Your licence is verified" : "Get your licence checked"
                                    color: root.mine && root.mine.verified ? Theme.palette.success : Theme.palette.text
                                }
                                Caption {
                                    visible: !!root.mine && root.mine.verified === true
                                    text: root.credentialLine(root.mine) + ". Clients see a Verified badge on your listing."
                                }
                                Caption {
                                    visible: !(root.mine && root.mine.verified)
                                    text: root.activeVerifiers.length ? "Verified therapists are shown first, and most clients look for the badge. Send your licence details to a licensing body below. They're encrypted on the way, checked by the body with your regulator, and the number itself is never published." : "No licensing bodies have joined this Ghostmind network yet, so licences can't be checked for now. Your listing is still visible to clients."
                                }
                                ColumnLayout {
                                    visible: !(root.mine && root.mine.verified) && root.activeVerifiers.length > 0
                                    Layout.fillWidth: true
                                    spacing: Theme.spacing.small
                                    FieldLabel {
                                        text: "Licence or registration number"
                                    }
                                    Field {
                                        id: licField
                                        placeholderText: "As it appears on your regulator's register"
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: Theme.spacing.small
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: Theme.spacing.small
                                            FieldLabel {
                                                text: "Where it was issued"
                                            }
                                            Field {
                                                id: jurField
                                                placeholderText: "e.g. NL, California"
                                            }
                                        }
                                        ColumnLayout {
                                            spacing: Theme.spacing.small
                                            FieldLabel {
                                                text: "Expiry date (optional)"
                                            }
                                            Field {
                                                id: expField
                                                Layout.preferredWidth: 170
                                                Layout.fillWidth: false
                                                placeholderText: "YYYY-MM-DD"
                                            }
                                        }
                                    }
                                    FieldLabel {
                                        text: "Send to"
                                    }
                                    Repeater {
                                        model: root.activeVerifiers
                                        delegate: Rectangle {
                                            required property var modelData
                                            Layout.fillWidth: true
                                            implicitHeight: vrow.implicitHeight + 2 * Theme.spacing.medium
                                            radius: Theme.spacing.radiusLarge
                                            color: Theme.palette.backgroundSecondary
                                            RowLayout {
                                                id: vrow
                                                anchors.fill: parent
                                                anchors.margins: Theme.spacing.medium
                                                spacing: Theme.spacing.medium
                                                Avatar {
                                                    seed: modelData.account
                                                    name: modelData.name
                                                    size: 32
                                                }
                                                LogosText {
                                                    Layout.fillWidth: true
                                                    text: modelData.name
                                                    color: Theme.palette.text
                                                    font.pixelSize: Theme.typography.primaryText
                                                    font.weight: Theme.typography.weightBold
                                                    elide: Text.ElideRight
                                                }
                                                Pill {
                                                    text: "Send for checking"
                                                    secondary: true
                                                    can: root.ready && licField.text.trim().length > 0 && jurField.text.trim().length > 0
                                                    onClicked: root.callArgs("requestVerification", [modelData.account, licField.text, jurField.text, expField.text], function (r) {
                                                        if (r && r.ok)
                                                            root.openConv(r.conversation);
                                                    })
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            Card {
                                pad: Theme.spacing.large
                                Title {
                                    text: "How payments and reviews work"
                                    font.pixelSize: Theme.typography.primaryText
                                }
                                Caption {
                                    text: "Clients pay into your private account, so neither of you shows up on the chain. When a payment arrives, Ghostmind confirms it for you and lets that client leave one review. Ghostmind's fee for that is " + root.feeText + ", taken from your public account. Only paying clients can review you, so nobody can flood you with fake ones. Three reports from paying clients hide your listing until the council reviews it."
                                }
                            }
                        }
                    }

                    // ── Licence checks (verifier) ──
                    ColumnLayout {
                        visible: root.page === "checks"
                        Layout.fillWidth: true
                        spacing: Theme.spacing.large
                        Card {
                            Title {
                                text: "Licence check requests"
                            }
                            Caption {
                                text: "Ghostmind's council appointed you to check therapists' licences. Therapists send their details to you encrypted. Check them with the regulator, then confirm in the conversation. Your confirmation is public, so anyone can see who vouched for whom."
                            }
                            Caption {
                                visible: root.conversations.filter(function (c) { return c.role === "host" && c.purpose === "verification"; }).length === 0
                                text: "No requests yet."
                                color: Theme.palette.textSecondary
                            }
                            Repeater {
                                model: root.conversations.filter(function (c) { return c.role === "host" && c.purpose === "verification"; })
                                delegate: Rectangle {
                                    required property var modelData
                                    readonly property var t: root.therapistOf(modelData.peerAccount)
                                    Layout.fillWidth: true
                                    implicitHeight: reqRow.implicitHeight + 2 * Theme.spacing.medium
                                    radius: Theme.spacing.radiusLarge
                                    color: Theme.palette.backgroundSecondary
                                    RowLayout {
                                        id: reqRow
                                        anchors.fill: parent
                                        anchors.margins: Theme.spacing.medium
                                        spacing: Theme.spacing.medium
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            RowLayout {
                                                spacing: Theme.spacing.small
                                                LogosText {
                                                    text: modelData.peerName
                                                    color: Theme.palette.text
                                                    font.pixelSize: Theme.typography.primaryText
                                                    font.weight: Theme.typography.weightBold
                                                }
                                                Chip {
                                                    text: t && t.verified ? "✓ Verified" : "To check"
                                                    tint: t && t.verified ? Theme.palette.success : Theme.palette.warning
                                                }
                                            }
                                            Caption {
                                                text: modelData.licence ? modelData.licence + " · " + modelData.jurisdiction + (modelData.expires ? " · expires " + modelData.expires : "") : "Waiting for details"
                                            }
                                        }
                                        Pill {
                                            text: "Open"
                                            secondary: true
                                            onClicked: root.openConv(modelData.id)
                                        }
                                    }
                                }
                            }
                        }
                        Card {
                            pad: Theme.spacing.large
                            Title {
                                text: "Confirm or withdraw by address"
                                font.pixelSize: Theme.typography.primaryText
                            }
                            Caption {
                                text: "For licences you checked outside Ghostmind."
                            }
                            Field {
                                id: vAddr
                                placeholderText: "Therapist's LEZ account"
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                spacing: Theme.spacing.small
                                Field {
                                    id: vLic
                                    placeholderText: "Licence number you checked"
                                }
                                Field {
                                    id: vJur
                                    placeholderText: "Jurisdiction"
                                }
                                Field {
                                    id: vExp
                                    Layout.preferredWidth: 150
                                    Layout.fillWidth: false
                                    placeholderText: "Expires YYYY-MM-DD"
                                }
                            }
                            RowLayout {
                                spacing: Theme.spacing.medium
                                Pill {
                                    text: "Confirm licence"
                                    can: root.ready && vAddr.text.trim().length > 0 && vLic.text.trim().length > 0 && vJur.text.trim().length > 0
                                    onClicked: root.callArgs("verifyTherapist", [vAddr.text, vLic.text, vJur.text, vExp.text])
                                }
                                Pill {
                                    text: "Withdraw my verification"
                                    danger: true
                                    can: root.ready && vAddr.text.trim().length > 0
                                    onClicked: root.callArgs("revokeTherapist", [vAddr.text])
                                }
                            }
                        }
                    }

                    // ── Council ──
                    ColumnLayout {
                        visible: root.page === "council"
                        Layout.fillWidth: true
                        spacing: Theme.spacing.large
                        Card {
                            visible: root.program.hasCouncil === false
                            Title {
                                text: "This Ghostmind registry has no council yet"
                            }
                            Caption {
                                text: "The council appoints licensing bodies and can suspend therapists. The first wallet to claim it becomes the council. Normally whoever deployed the registry does this straight away."
                            }
                            Pill {
                                text: "Become the council"
                                can: root.ready && !!root.program.configured
                                onClicked: root.callArgs("initCouncil", [])
                            }
                        }
                        Card {
                            visible: root.isCouncil
                            Title {
                                text: "Licensing bodies"
                            }
                            Caption {
                                text: "Organisations that check therapists' licences, such as a national register or professional association. To appoint one, you need its LEZ account and chat key. Both are under Wallet › Advanced in its Ghostmind app."
                            }
                            Repeater {
                                model: root.verifiers
                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: Theme.spacing.medium
                                    Avatar {
                                        seed: modelData.account
                                        name: modelData.name
                                        size: 32
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        RowLayout {
                                            spacing: Theme.spacing.small
                                            LogosText {
                                                text: modelData.name
                                                color: Theme.palette.text
                                                font.pixelSize: Theme.typography.primaryText
                                                font.weight: Theme.typography.weightBold
                                            }
                                            Chip {
                                                text: modelData.active ? "Active" : "Disabled"
                                                tint: modelData.active ? Theme.palette.success : Theme.palette.textTertiary
                                            }
                                        }
                                        Caption {
                                            text: modelData.account
                                            font.family: "Menlo"
                                            elide: Text.ElideMiddle
                                            wrapMode: Text.NoWrap
                                        }
                                    }
                                    Pill {
                                        text: modelData.active ? "Disable" : "Enable"
                                        secondary: true
                                        can: root.ready
                                        onClicked: root.callArgs("setVerifier", [modelData.account, modelData.name, modelData.chatKey, !modelData.active])
                                    }
                                }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                implicitHeight: 1
                                color: Theme.palette.borderSecondary
                            }
                            FieldLabel {
                                text: "Appoint a licensing body"
                            }
                            Field {
                                id: cvName
                                placeholderText: "Name, e.g. Dutch Psychologists' Register"
                            }
                            Field {
                                id: cvAddr
                                placeholderText: "Its LEZ account"
                            }
                            Field {
                                id: cvKey
                                placeholderText: "Its chat key (64 characters)"
                            }
                            Pill {
                                text: "Appoint"
                                can: root.ready && cvAddr.text.trim().length > 0 && cvName.text.trim().length > 0 && cvKey.text.trim().length === 64
                                onClicked: root.callArgs("setVerifier", [cvAddr.text.trim(), cvName.text.trim(), cvKey.text.trim(), true], function (r) {
                                    if (r && r.ok) {
                                        cvAddr.text = "";
                                        cvName.text = "";
                                        cvKey.text = "";
                                    }
                                })
                            }
                        }
                        Card {
                            visible: root.isCouncil
                            Title {
                                text: "Therapists"
                            }
                            Caption {
                                text: "Suspending hides a therapist from clients and stops new paid sessions. Reinstating brings them back and clears their reports. Therapists hidden by client reports appear here for review."
                            }
                            Field {
                                id: suspendReason
                                placeholderText: "Reason shown to the therapist when you suspend (optional)"
                            }
                            Repeater {
                                model: root.therapists
                                delegate: RowLayout {
                                    required property var modelData
                                    Layout.fillWidth: true
                                    spacing: Theme.spacing.medium
                                    Avatar {
                                        seed: modelData.account
                                        name: modelData.name
                                        size: 32
                                    }
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 0
                                        RowLayout {
                                            spacing: Theme.spacing.small
                                            LogosText {
                                                text: modelData.name
                                                color: Theme.palette.text
                                                font.pixelSize: Theme.typography.primaryText
                                                font.weight: Theme.typography.weightBold
                                            }
                                            Chip {
                                                text: root.statusText(modelData)
                                                tint: modelData.status === 0 ? Theme.palette.success : Theme.palette.error
                                            }
                                        }
                                        Caption {
                                            text: root.ratingLine(modelData) + " · " + (modelData.reports || 0) + (modelData.reports === 1 ? " report" : " reports") + (modelData.reason ? " · " + modelData.reason : "")
                                        }
                                    }
                                    Pill {
                                        visible: modelData.status === 0
                                        text: "Suspend"
                                        danger: true
                                        can: root.ready
                                        onClicked: root.callArgs("suspendTherapist", [modelData.account, suspendReason.text])
                                    }
                                    Pill {
                                        visible: modelData.status !== 0
                                        text: "Reinstate"
                                        secondary: true
                                        can: root.ready
                                        onClicked: root.callArgs("reinstateTherapist", [modelData.account])
                                    }
                                }
                            }
                        }
                    }
                }

                Item {
                    implicitHeight: Theme.spacing.xxlarge
                }
            }
        }
    }
}
