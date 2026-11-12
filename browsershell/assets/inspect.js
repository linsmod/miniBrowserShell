/*
 * inspect.js - the page half of the host-side element marker.
 *
 * It never inserts a node: an injected highlight can stretch the document,
 * create scrollbars, or be captured by an ancestor's transform. The shell owns
 * the overlay window; this script only reports the target's rect and a label
 * over agavePostMessage('__agb_inspect__<device rect>|<css rect>|<label>|<sel>').
 *
 * The device rect is scaled here, with this window's own devicePixelRatio, not
 * with the shell's 125%/etc. guess: on the very first frames the engine still
 * reports a 1.0 ratio (it re-lays out on the first resize), so a host-side
 * multiplication lands the mark at 0.8x of the right place until the window is
 * resized. The page ratio is the only authority for both directions.
 */
(function () {
    if (window.__mbInspect) {
        window.__mbInspect.stop();
    }

    var last = "";
    var target = null;

    function dpr() {
        return window.devicePixelRatio || 1;
    }

    function deviceRect(r) {
        var d = dpr();

        return Math.round(r.left * d) + "," + Math.round(r.top * d) + "," +
               Math.round(r.width * d) + "," + Math.round(r.height * d);
    }

    function cssRect(r) {
        return Math.round(r.left) + "," + Math.round(r.top) + "," +
               Math.round(r.width) + "," + Math.round(r.height);
    }

    function selectorOf(el) {
        if (!el || !el.tagName) {
            return "";
        }
        if (el.id) {
            return el.tagName.toLowerCase() + "#" + el.id;
        }

        var parts = [];
        var node = el;
        var depth = 0;

        while (node && node.nodeType === 1 && depth < 4) {
            var tag = node.tagName.toLowerCase();
            var parent = node.parentNode;
            var index = 1;

            if (parent) {
                for (var i = 0; i < parent.children.length; i++) {
                    var sib = parent.children[i];
                    if (sib === node) {
                        break;
                    }
                    if (sib.tagName === node.tagName) {
                        index++;
                    }
                }
            }
            parts.unshift(tag + ":nth-of-type(" + index + ")");
            node = parent;
            depth++;
        }

        return parts.join(" > ");
    }

    function labelOf(el) {
        var tag = el.tagName ? el.tagName.toLowerCase() : "?";
        var id = el.id ? "#" + el.id : "";
        var cls = "";
        var r = el.getBoundingClientRect();

        if (typeof el.className === "string" && el.className) {
            cls = "." + el.className.trim().split(/\s+/).slice(0, 2).join(".");
        }

        return tag + id + cls + "  " + Math.round(r.width) + "x" + Math.round(r.height);
    }

    function report(el) {
        var r, payload;

        if (!el || !el.getBoundingClientRect) {
            return;
        }

        r = el.getBoundingClientRect();
        payload = deviceRect(r) + "|" + cssRect(r) + "|" + dpr() + "|" +
                  labelOf(el) + "|" + selectorOf(el);

        if (payload === last) {
            return;
        }

        last = payload;
        try {
            agavePostMessage("__agb_inspect__" + payload);
        } catch (e) {}
    }

    function onMove(e) {
        if (!e.target || !e.target.getBoundingClientRect) {
            return;
        }
        target = e.target;
        report(target);
    }

    /* Host-driven hover: while inspecting the page never sees a mousemove, so the
     * host asks for the element under a point instead. The point arrives in
     * device pixels; elementFromPoint wants CSS pixels. */
    function probeAt(x, y) {
        var el;
        var d = dpr();

        try {
            el = document.elementFromPoint(x / d, y / d);
        } catch (e) {
            return;
        }

        if (el && el.getBoundingClientRect) {
            target = el;
            report(el);
        }
    }

    function onScroll() {
        if (target) {
            report(target);
        }
    }

    function stop() {
        document.removeEventListener("mousemove", onMove, true);
        document.removeEventListener("scroll", onScroll, true);
        window.removeEventListener("resize", onScroll, true);
        window.__mbInspect = null;
    }

    window.__mbInspect = { stop: stop, report: report, probe: probeAt };

    document.addEventListener("mousemove", onMove, true);
    document.addEventListener("scroll", onScroll, true);
    window.addEventListener("resize", onScroll, true);
})();
