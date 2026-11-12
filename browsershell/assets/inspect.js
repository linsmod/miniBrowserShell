/*
 * inspect.js - the page half of the host-side element marker.
 *
 * It never inserts a node: an injected highlight can stretch the document,
 * create scrollbars, or be captured by an ancestor's transform. The shell owns
 * the overlay window; this script only reports the target's viewport rect and a
 * label over agavePostMessage('__agb_inspect__x,y,w,h|label').
 */
(function () {
    if (window.__mbInspect) {
        window.__mbInspect.stop();
    }

    var last = "";
    var target = null;

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
        payload = Math.round(r.left) + "," + Math.round(r.top) + "," +
                  Math.round(r.width) + "," + Math.round(r.height) + "|" +
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

    /* Host-driven hover: with the inspect shield on, the page never sees a
     * mousemove, so the host asks for the element under a point instead. */
    function probeAt(x, y) {
        var el;

        try {
            el = document.elementFromPoint(x, y);
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
