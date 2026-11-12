/*
 * inspector.js - hover element inspector for the agave Win32 shell.
 *
 * Injected from the shell (right-click menu / "=" menu -> 注入 inspector.js).
 * It is deliberately plain ES5 and dependency-free: it has to run on the
 * engine's own DOM, and it is evaluated as one script, not loaded as a module.
 *
 * What it does:
 *   - follows the mouse, finds the element under the cursor
 *   - draws an absolutely positioned outline + a semi-transparent info panel
 *     (position:absolute, z-index max, appended under <html>) so it never takes
 *     part in the document flow and never shifts the page
 *   - right-click opens an in-page menu for the element under the cursor:
 *     copy element info (one line of JSON - page URL, selector, identity,
 *     text, viewport + document rect, scroll, viewport size, key styles) so a
 *     bug report or an agent knows which element it is and where it sits;
 *     also selector / bounds / text / HTML, up to the parent, exit
 *   - Esc first closes the menu, then removes the inspector; injecting it
 *     again toggles it off
 *
 * Engine notes (do not "fix" these without checking the engine):
 *   - getBoundingClientRect() returns VIEWPORT coordinates (R82; it was
 *     document space before that). The root's own rect is in the same space,
 *     so a box anchored on a rect subtracts the root rect directly, while a
 *     panel anchored on a mouse point has to go through the scroll offsets.
 *   - pointer-events is not honoured on HTML boxes, so the overlay would eat
 *     the hover. elementFromPoint() is therefore called with the overlay
 *     display:none; the page sees one synchronous style flip, no repaint.
 *   - right-click: AntRuntime::dispatchEvent() runs the page's JS listeners
 *     BEFORE WebCore dispatches default handlers, so preventDefault() here does
 *     stop ContextMenuController from popping the shell's native menu. The
 *     menu lives in the page, it is not a Win32 TrackPopupMenu.
 *   - clipboard: there is no document.execCommand('copy') in this engine, so the
 *     host global __agbClipboardSetText() (AntRuntime.cpp) writes CF_UNICODETEXT
 *     through the control's pasteboard callback. navigator.clipboard.writeText
 *     is a prelude polyfill over the same primitive.
 *   - scroll offsets come from the mousemove event (pageX - clientX). window
 *     pageYOffset / documentElement.scrollTop are live since R82, but the
 *     event delta is the value the event itself was dispatched with, so the
 *     two can never disagree mid-frame.
 */
(function () {
  'use strict';

  var W = window;
  var D = document;
  var KEY = '__agaveInspector';

  /* Re-injection is a toggle: the menu item is cheap to hit twice. */
  if (W[KEY]) {
    W[KEY].destroy();
    return;
  }

  var ID = '__agave_inspector__';
  var Z = '2147483647';
  var MENU_ROWS = 10;
  var st = {
    root: null,
    box: null,
    tip: null,
    line1: null,
    line2: null,
    line3: null,
    line4: null,
    menu: null,
    rows: [],
    toast: null,
    element: null,
    pinned: null,
    menuOpen: false,
    menuX: 0,
    menuY: 0,
    actions: [],
    labels: [],
    active: -1,
    toastTimer: 0,
    /* Hover updates are suspended until the cursor leaves the menu anchor:
     * otherwise the next mousemove immediately re-selects whatever sits under
     * the (now closed) menu and "Select parent" would look like a no-op. */
    holdX: null,
    holdY: null,
    pending: false,
    cursorX: 0,
    cursorY: 0,
    lastX: -1,
    lastY: -1,
    /* Scroll offset derived from the event itself (pageX - clientX), so the
     * placement is consistent with the coordinates the event carried. */
    scrollLeft: 0,
    scrollTop: 0,
    haveScroll: false
  };

  function setStyle(el, props) {
    for (var k in props) {
      if (Object.prototype.hasOwnProperty.call(props, k)) {
        try { el.style[k] = props[k]; } catch (e) {}
      }
    }
  }

  function make(tag, props) {
    var el = D.createElement(tag);
    if (props) setStyle(el, props);
    return el;
  }

  function setText(el, s) {
    var v = String(s == null ? '' : s);
    if (el.textContent !== v) el.textContent = v;
  }

  function num(v) {
    return typeof v === 'number' && isFinite(v) ? v : 0;
  }

  function build() {
    var root = make('div', {
      position: 'absolute',
      left: '0px', top: '0px', width: '0px', height: '0px',
      margin: '0px', padding: '0px', border: '0px',
      backgroundColor: 'transparent',
      zIndex: Z, display: 'block', overflow: 'visible',
      pointerEvents: 'none'
    });
    root.id = ID;
    root.setAttribute('data-agave-inspector', '1');

    /* Outline. The 2px border sits outside the element's border box, which
     * also makes very small targets (1x1 tracking pixels) visible. */
    var box = make('div', {
      position: 'absolute', left: '0px', top: '0px',
      width: '0px', height: '0px',
      border: '2px solid rgba(56, 189, 248, 0.95)',
      backgroundColor: 'rgba(56, 189, 248, 0.16)',
      boxSizing: 'content-box',
      zIndex: Z, display: 'none'
    });

    var tip = make('div', {
      position: 'absolute', left: '0px', top: '0px',
      padding: '5px 8px',
      border: '1px solid rgba(56, 189, 248, 0.55)',
      backgroundColor: 'rgba(15, 23, 42, 0.86)',
      color: '#e2e8f0',
      fontSize: '11px',
      fontFamily: 'Consolas, "Courier New", monospace',
      lineHeight: '1.5',
      whiteSpace: 'nowrap',
      borderRadius: '4px',
      zIndex: Z, display: 'none'
    });

    var line1 = make('div', { color: '#7dd3fc' });
    var line2 = make('div', { color: '#e2e8f0' });
    var line3 = make('div', { color: '#94a3b8', display: 'none' });
    var line4 = make('div', { color: '#64748b' });

    tip.appendChild(line1);
    tip.appendChild(line2);
    tip.appendChild(line3);
    tip.appendChild(line4);

    /* The menu is a fixed pool of rows created once and re-labelled on every
     * open. Removing nodes while the engine is still dispatching the click
     * that closed the menu is not worth the risk (a detached node has no
     * document, and EventTargetNode::dispatchEvent() dereferences it). */
    var menu = make('div', {
      position: 'absolute', left: '0px', top: '0px',
      width: '188px', padding: '4px 0px',
      border: '1px solid rgba(56, 189, 248, 0.55)',
      backgroundColor: 'rgba(15, 23, 42, 0.97)',
      color: '#e2e8f0',
      fontSize: '12px',
      fontFamily: 'Consolas, "Courier New", monospace',
      lineHeight: '1.4',
      borderRadius: '5px',
      boxSizing: 'border-box',
      zIndex: Z, display: 'none', overflow: 'hidden'
    });

    var rows = [];
    for (var i = 0; i < MENU_ROWS; ++i) {
      var row = make('div', {
        padding: '4px 10px',
        whiteSpace: 'nowrap',
        backgroundColor: 'transparent',
        color: '#e2e8f0',
        display: 'none'
      });
      menu.appendChild(row);
      rows.push(row);
    }

    var toast = make('div', {
      position: 'absolute', left: '0px', top: '0px',
      padding: '4px 8px',
      border: '1px solid rgba(74, 222, 128, 0.55)',
      backgroundColor: 'rgba(6, 78, 59, 0.92)',
      color: '#dcfce7',
      fontSize: '11px',
      fontFamily: 'Consolas, "Courier New", monospace',
      whiteSpace: 'nowrap',
      borderRadius: '4px',
      zIndex: Z, display: 'none'
    });

    root.appendChild(box);
    root.appendChild(tip);
    root.appendChild(menu);
    root.appendChild(toast);
    (D.documentElement || D.body).appendChild(root);

    /* Force layout once so the first getBoundingClientRect() below is real:
     * the bridge's getBoundingClientRect() does not update layout itself, but
     * Element::offsetWidth() does. */
    void root.offsetWidth;

    st.root = root;
    st.box = box;
    st.tip = tip;
    st.line1 = line1;
    st.line2 = line2;
    st.line3 = line3;
    st.line4 = line4;
    st.menu = menu;
    st.rows = rows;
    st.toast = toast;
  }

  function scrollX() {
    var de = D.documentElement;
    var body = D.body;
    return (de && de.scrollLeft) || (body && body.scrollLeft) || 0;
  }

  function scrollY() {
    var de = D.documentElement;
    var body = D.body;
    return (de && de.scrollTop) || (body && body.scrollTop) || 0;
  }

  /* The element under the cursor with our own overlay out of the way. The
   * overlay is hidden and restored inside one synchronous task, so nothing is
   * painted in between. */
  function hitTest(clientX, clientY) {
    var root = st.root;
    var previous = root.style.display;
    var el = null;

    root.style.display = 'none';
    try {
      el = D.elementFromPoint(clientX, clientY);
    } catch (e) {
      el = null;
    }
    root.style.display = previous || 'block';

    while (el && el.nodeType !== 1) el = el.parentNode;
    return el || null;
  }

  /* Hit test that must NOT hide the overlay: used while the menu is open, to
   * find out which of our own rows is under the cursor. */
  function hitOwn(clientX, clientY) {
    try {
      return D.elementFromPoint(clientX, clientY) || null;
    } catch (e) {
      return null;
    }
  }

  function isInside(el) {
    for (var n = el; n; n = n.parentNode) {
      if (n === st.root) return true;
    }
    return false;
  }

  function describe(el) {
    var name = String(el.tagName || '?').toLowerCase();
    var classes = '';
    var truncated = false;

    if (el.id) name += '#' + el.id;

    try { classes = el.getAttribute('class') || ''; } catch (e) { classes = ''; }
    if (classes) {
      var parts = String(classes).split(/\s+/);
      var shown = 0;
      for (var i = 0; i < parts.length; ++i) {
        if (!parts[i]) continue;
        if (shown < 3) {
          name += '.' + parts[i];
        } else {
          truncated = true;
        }
        ++shown;
      }
    }

    if (truncated) name += '...';
    if (name.length > 78) name = name.slice(0, 75) + '...';
    return name;
  }

  function describeStyle(el) {
    var out = [];
    try {
      var cs = W.getComputedStyle(el, null);
      var display = cs.getPropertyValue('display');
      var position = cs.getPropertyValue('position');
      if (display) out.push('display:' + display);
      if (position) out.push('position:' + position);
    } catch (e) {}
    return out.join('   ');
  }

  function rectOf(el) {
    var rect = null;

    try { rect = el.getBoundingClientRect(); } catch (e) { rect = null; }
    return rect;
  }

  /* Root is positioned at (0,0) inside its containing block, which is not
   * necessarily the document origin (body margin, positioned ancestor), so
   * everything inside it is shifted by where the root actually landed.
   * The layout is forced first: getBoundingClientRect() does not lay out, and
   * hitTest() just toggled our display (and the page may have been scrolled
   * since), so the rect would otherwise be the pre-toggle one. */
  function anchorOffset() {
    void st.root.offsetWidth;

    var anchor = st.root.getBoundingClientRect();
    return { x: num(anchor.left), y: num(anchor.top) };
  }

  /* Places an absolutely positioned panel next to the cursor, flipped when it
   * would run past the viewport edge. Coordinates are document space (that is
   * what the engine's rects and our children use). */
  function positionPanel(panel, clientX, clientY, gapX, gapY) {
    var anchor = anchorOffset();
    var sx = st.haveScroll ? st.scrollLeft : scrollX();
    var sy = st.haveScroll ? st.scrollTop : scrollY();
    var vw = W.innerWidth || 0;
    var vh = W.innerHeight || 0;
    var pw = 0;
    var ph = 0;
    var px;
    var py;

    /* The panel may still be display:none from the previous hide(); make it
     * measurable before deciding where it goes, otherwise the first placement
     * uses a 0x0 size and can overflow the viewport edge. */
    panel.style.display = 'block';
    pw = panel.offsetWidth;
    ph = panel.offsetHeight;

    px = clientX + sx + gapX;
    py = clientY + sy + gapY;

    if (vw && px + pw > sx + vw - 8) px = clientX + sx - pw - gapX;
    if (vh && py + ph > sy + vh - 8) py = clientY + sy - ph - gapY;
    if (px < sx + 4) px = sx + 4;
    if (py < sy + 4) py = sy + 4;

    /* px/py are document space (the clamping above is relative to the scrolled
     * viewport); the root's rect is viewport space. Convert, then take the
     * root out of the way: without the -sx/-sy the panel lands a whole scroll
     * distance too far down/right. */
    setStyle(panel, {
      left: (px - anchor.x - sx) + 'px',
      top: (py - anchor.y - sy) + 'px',
      display: 'block'
    });
  }

  function placeBox(rect) {
    var anchor = anchorOffset();

    /* Shift by the border width: the outline is a content-box div with a 2px
     * border, so without the shift it grows down/right instead of ringing the
     * element evenly. */
    setStyle(st.box, {
      left: (num(rect.left) - anchor.x - 2) + 'px',
      top: (num(rect.top) - anchor.y - 2) + 'px',
      width: Math.max(0, num(rect.width)) + 'px',
      height: Math.max(0, num(rect.height)) + 'px',
      display: 'block'
    });
  }

  function show(el, clientX, clientY) {
    var rect = rectOf(el);

    st.element = el;
    if (!rect) {
      hide();
      return;
    }

    setText(st.line1, describe(el));
    setText(st.line2, Math.round(num(rect.width)) + ' x ' + Math.round(num(rect.height)) +
            '  at ' + Math.round(num(rect.left)) + ', ' + Math.round(num(rect.top)));

    var extra = describeStyle(el);
    if (extra) {
      setText(st.line3, extra);
      st.line3.style.display = 'block';
    } else {
      st.line3.style.display = 'none';
    }

    placeBox(rect);
    positionPanel(st.tip, clientX, clientY, 16, 18);
  }

  function hide() {
    st.element = null;
    if (st.box) st.box.style.display = 'none';
    if (st.tip) st.tip.style.display = 'none';
    closeMenu();
  }

  /* ---------------------------------------------------------------- menu -- */

  /* Short CSS path: #id wins, otherwise tag + up to two classes and
   * :nth-of-type() only when the parent repeats the tag. Purely textual (it is
   * copied out, not fed back to querySelector, whose coverage is narrower). */
  function selectorFor(el) {
    var parts = [];
    var node = el;
    var depth = 0;

    while (node && node.nodeType === 1 && depth < 8) {
      var tag = String(node.tagName || '').toLowerCase();
      var part;
      var parent;
      var index = 1;
      var count = 0;
      var sibling;

      if (!tag) break;
      if (tag === 'html') break;

      if (node.id) {
        parts.unshift(tag + '#' + node.id);
        break;
      }

      part = tag;

      var classes = '';
      try { classes = node.getAttribute('class') || ''; } catch (e) { classes = ''; }
      if (classes) {
        var list = String(classes).split(/\s+/);
        var used = 0;
        for (var i = 0; i < list.length && used < 2; ++i) {
          if (list[i]) {
            part += '.' + list[i];
            ++used;
          }
        }
      }

      if (tag === 'body') {
        parts.unshift(part);
        break;
      }

      parent = node.parentNode;
      for (sibling = parent ? parent.firstChild : null; sibling; sibling = sibling.nextSibling) {
        if (sibling.nodeType === 1 && String(sibling.tagName || '').toLowerCase() === tag) {
          ++count;
          if (sibling === node) index = count;
        }
      }
      if (count > 1) part += ':nth-of-type(' + index + ')';

      parts.unshift(part);
      node = parent;
      ++depth;
    }

    return parts.length ? parts.join(' > ') : String(el.tagName || '').toLowerCase();
  }

  function boundsFor(el) {
    var rect = rectOf(el);

    if (!rect) return '';
    return 'left=' + Math.round(num(rect.left)) +
           ' top=' + Math.round(num(rect.top)) +
           ' width=' + Math.round(num(rect.width)) +
           ' height=' + Math.round(num(rect.height));
  }

  function textFor(el) {
    var text = '';

    try { text = el.textContent || ''; } catch (e) { text = ''; }
    text = String(text).replace(/\s+/g, ' ');
    text = text.replace(/^ +/, '').replace(/ +$/, '');
    if (text.length > 4000) text = text.slice(0, 4000) + '...';
    return text;
  }

  function htmlFor(el) {
    try { return el.outerHTML || ''; } catch (e) { return ''; }
  }

  /* ------------------------------------------------- element info (JSON) -- */
  /* "Copy element info" produces one line of JSON: enough for another tool (or
   * an agent) to know WHICH element this is and WHERE it sits, without seeing
   * the page. The rects are named after the call that produces them:
   * `getBoundingClientRect` is exactly what that method returned (viewport
   * space, what a screenshot shows), `getBoundingClientRectInPage` is the same
   * rect plus `page.scroll` (page space, what layout/CSS uses, stable while
   * scrolling). */

  function hex4(n) {
    var digits = '0123456789abcdef';
    var v = n & 0xFFFF;

    return '\\u' + digits.charAt((v >> 12) & 15) + digits.charAt((v >> 8) & 15) +
           digits.charAt((v >> 4) & 15) + digits.charAt(v & 15);
  }

  function jsonString(value) {
    var s = String(value == null ? '' : value);
    var out = '"';
    var i;
    var c;
    var code;

    for (i = 0; i < s.length; ++i) {
      c = s.charAt(i);
      code = s.charCodeAt(i);

      if (c === '"') out += '\\"';
      else if (c === '\\') out += '\\\\';
      else if (c === '\n') out += '\\n';
      else if (c === '\r') out += '\\r';
      else if (c === '\t') out += '\\t';
      else if (code < 0x20) out += hex4(code);
      else out += c;
    }

    return out + '"';
  }

  function intText(value) {
    return String(Math.round(num(value)));
  }

  function styleJson(el) {
    var props = ['display', 'position', 'font-size', 'line-height', 'color', 'background-color'];
    var parts = [];
    var i;

    try {
      var cs = W.getComputedStyle(el, null);

      for (i = 0; i < props.length; ++i) {
        var v = cs.getPropertyValue(props[i]);

        if (v) parts.push(jsonString(props[i]) + ':' + jsonString(v));
      }
    } catch (e) {}

    return '{' + parts.join(',') + '}';
  }

  function elementInfo(el) {
    var rect = rectOf(el) || { left: 0, top: 0, width: 0, height: 0 };
    var sx = st.haveScroll ? st.scrollLeft : scrollX();
    var sy = st.haveScroll ? st.scrollTop : scrollY();
    var vw = W.innerWidth || 0;
    var vh = W.innerHeight || 0;
    var left = num(rect.left);
    var top = num(rect.top);
    var width = num(rect.width);
    var height = num(rect.height);
    var text = textFor(el);
    var html = htmlFor(el);
    var classes = '';
    var id = '';
    var tag = String(el.tagName || '').toLowerCase();
    var inViewport = 0;

    if (text.length > 200) text = text.slice(0, 200) + '...';
    if (html.length > 400) html = html.slice(0, 400) + '...';

    try { id = el.id || ''; } catch (e) { id = ''; }
    try { classes = el.getAttribute('class') || ''; } catch (e) { classes = ''; }

    if (width > 0 && height > 0 && left + width > 0 && top + height > 0 &&
        left < vw && top < vh) {
      inViewport = 1;
    }

    return '{' +
      '"kind":"agave-element","version":1,' +
      '"page":{"url":' + jsonString(D.URL || '') +
        ',"title":' + jsonString(D.title || '') +
        ',"viewport":{"width":' + intText(vw) + ',"height":' + intText(vh) + '}' +
        ',"scroll":{"x":' + intText(sx) + ',"y":' + intText(sy) + '}' +
      '},' +
      '"element":{"tag":' + jsonString(tag) +
        ',"id":' + jsonString(id) +
        ',"class":' + jsonString(classes) +
        ',"selector":' + jsonString(selectorFor(el)) +
        ',"text":' + jsonString(text) +
        ',"outerHTML":' + jsonString(html) +
        ',"inViewport":' + (inViewport ? 'true' : 'false') +
        /* Named after the call that produces it, so the reader does not have
         * to guess the coordinate space; the second one is the same rect
         * expressed in page space (add the scroll below). */
        ',"getBoundingClientRect":{"left":' + intText(left) + ',"top":' + intText(top) +
          ',"width":' + intText(width) + ',"height":' + intText(height) + '}' +
        ',"getBoundingClientRectInPage":{"left":' + intText(left + sx) + ',"top":' + intText(top + sy) +
          ',"width":' + intText(width) + ',"height":' + intText(height) + '}' +
        ',"style":' + styleJson(el) +
      '}' +
    '}';
  }

  function parentElementOf(el) {
    var n = el.parentNode;

    while (n && n.nodeType !== 1) n = n.parentNode;
    return n || null;
  }

  /* No document.execCommand('copy') in this engine: the host primitive writes
   * the Win32 clipboard directly (see the file header). */
  function copyToClipboard(text) {
    var value = String(text == null ? '' : text);

    try {
      if (typeof W.__agbClipboardSetText === 'function') {
        W.__agbClipboardSetText(value);
        return true;
      }
    } catch (e) {}

    try {
      if (W.navigator && W.navigator.clipboard && W.navigator.clipboard.writeText) {
        W.navigator.clipboard.writeText(value);
        return true;
      }
    } catch (e) {}

    return false;
  }

  function toast(message) {
    if (!st.toast) return;

    setText(st.toast, message);
    positionPanel(st.toast, st.menuX, st.menuY, 16, 18);

    if (st.toastTimer) W.clearTimeout(st.toastTimer);
    st.toastTimer = W.setTimeout(function () {
      st.toastTimer = 0;
      if (st.toast) st.toast.style.display = 'none';
    }, 1500);
  }

  function copyAndToast(text, what) {
    var ok = copyToClipboard(text);

    toast((ok ? 'copied ' : 'copy failed: ') + what);
  }

  /* The cursor must leave the point the last mousemove reported before hover
   * tracking resumes, or "Select parent" / a copy would be undone by the very
   * next mousemove over the element still under the closed menu. Clicking a
   * row produces no mousemove, so the anchor is where the pointer already is. */
  function holdHover(x, y) {
    st.holdX = x;
    st.holdY = y;
  }

  function releaseHold(x, y) {
    var dx;
    var dy;

    if (st.holdX === null) return true;

    dx = x - st.holdX;
    dy = y - st.holdY;
    if (dx * dx + dy * dy < 144) return false;

    st.holdX = null;
    st.holdY = null;
    return true;
  }

  function menuItemsFor(el, clientX, clientY) {
    var items = [];
    var text = textFor(el);
    var html = htmlFor(el);
    var parent = parentElementOf(el);

    items.push({
      label: 'Copy element info',
      run: function () { copyAndToast(elementInfo(el), 'element info'); }
    });
    items.push({
      label: 'Copy selector',
      run: function () { copyAndToast(selectorFor(el), 'selector'); }
    });
    items.push({
      label: 'Copy bounds',
      run: function () { copyAndToast(boundsFor(el), 'bounds'); }
    });

    if (text) {
      items.push({
        label: 'Copy text',
        run: function () { copyAndToast(text, 'text'); }
      });
    }
    if (html) {
      items.push({
        label: 'Copy HTML',
        run: function () { copyAndToast(html, 'HTML'); }
      });
    }
    if (parent) {
      items.push({
        label: 'Select parent',
        run: function () {
          st.element = parent;
          show(parent, clientX, clientY);
        }
      });
    }

    items.push({
      label: 'Exit inspector',
      run: function () { destroy(); }
    });

    return items;
  }

  function setActiveRow(index) {
    for (var i = 0; i < st.rows.length; ++i) {
      if (!st.rows[i]) continue;
      st.rows[i].style.backgroundColor = (i === index && i < st.labels.length)
        ? 'rgba(56, 189, 248, 0.28)'
        : 'transparent';
    }
    st.active = index;
  }

  function openMenu(el, clientX, clientY) {
    var items;
    var i;

    if (!st.menu || !el) return null;

    items = menuItemsFor(el, clientX, clientY);
    if (items.length > MENU_ROWS) items = items.slice(0, MENU_ROWS);

    st.pinned = el;
    st.element = el;
    st.menuX = clientX;
    st.menuY = clientY;
    st.actions = [];
    st.labels = [];

    for (i = 0; i < st.rows.length; ++i) {
      var row = st.rows[i];

      if (!row) continue;
      if (i < items.length) {
        setText(row, items[i].label);
        row.style.display = 'block';
        row.style.backgroundColor = 'transparent';
        st.actions.push(items[i].run);
        st.labels.push(items[i].label);
      } else {
        row.style.display = 'none';
      }
    }

    /* Keep the outline on the target and get the info panel out of the way:
     * the menu opens at the cursor, right where the panel used to be. */
    var rect = rectOf(el);
    if (rect) placeBox(rect);
    st.tip.style.display = 'none';

    st.menuOpen = true;
    setActiveRow(-1);
    positionPanel(st.menu, clientX, clientY, 12, 14);

    return st.labels;
  }

  function closeMenu() {
    if (!st.menuOpen) return;

    st.menuOpen = false;
    st.pinned = null;
    st.active = -1;
    if (st.menu) st.menu.style.display = 'none';
    for (var i = 0; i < st.rows.length; ++i) {
      if (!st.rows[i]) continue;
      st.rows[i].style.display = 'none';
      st.rows[i].style.backgroundColor = 'transparent';
    }
    st.actions = [];
    st.labels = [];
  }

  function rowAt(clientX, clientY) {
    var el;

    if (!st.menuOpen) return -1;

    el = hitOwn(clientX, clientY);
    while (el && el !== st.root) {
      for (var i = 0; i < st.rows.length; ++i) {
        if (st.rows[i] === el) return i;
      }
      el = el.parentNode;
    }
    return -1;
  }

  function runMenuAction(index) {
    var fn = st.actions[index];

    if (!fn) return;

    /* Close first: the action may remove the inspector (Exit) or move the
     * selection (Select parent). */
    closeMenu();
    if (st.lastX >= 0) holdHover(st.lastX, st.lastY);
    else holdHover(st.menuX, st.menuY);
    try { fn(); } catch (e) {}
  }

  /* -------------------------------------------------------------- events -- */

  function update(clientX, clientY) {
    var el;

    if (!st.root) return;

    el = hitTest(clientX, clientY);
    if (!el) {
      hide();
      return;
    }

    /* Defensive: hitTest hides the overlay, so this should be unreachable,
     * but a page can move our nodes around. */
    if (isInside(el)) {
      hide();
      return;
    }

    show(el, clientX, clientY);
  }

  function onMouseMove(ev) {
    if (!ev || !st.root) return;

    var x = ev.clientX;
    var y = ev.clientY;

    if (x === st.lastX && y === st.lastY) return;
    st.lastX = x;
    st.lastY = y;
    st.cursorX = x;
    st.cursorY = y;

    /* The engine reports page coordinates (contents-space) too; the delta is
     * the scroll offset the event was dispatched with (window.pageYOffset is
     * live since R82, this just avoids a second source of truth). */
    if (typeof ev.pageX === 'number' && typeof ev.pageY === 'number') {
      st.scrollLeft = ev.pageX - x;
      st.scrollTop = ev.pageY - y;
      st.haveScroll = true;
    }

    if (!st.menuOpen && !releaseHold(x, y)) return;

    if (st.pending) return;
    st.pending = true;

    /* Coalesce bursts: one hit test per frame is plenty, and the engine's
     * hit test plus style flip is the expensive part. */
    W.setTimeout(function () {
      st.pending = false;
      if (!st.root) return;

      /* While the menu is open the selection is frozen and the overlay must
       * stay hit-testable, otherwise we could not tell which row is hovered. */
      if (st.menuOpen) {
        setActiveRow(rowAt(st.cursorX, st.cursorY));
        return;
      }

      update(st.cursorX, st.cursorY);
    }, 16);
  }

  /* Right-click: the inspector owns it while it is active. Opening the page
   * menu also cancels the engine's default context menu handler (which would
   * otherwise show the shell's native menu at the same time). */
  function onContextMenu(ev) {
    var x;
    var y;
    var el;

    if (!ev || !st.root) return;

    x = typeof ev.clientX === 'number' ? ev.clientX : st.cursorX;
    y = typeof ev.clientY === 'number' ? ev.clientY : st.cursorY;

    if (ev.preventDefault) ev.preventDefault();
    if (ev.stopPropagation) ev.stopPropagation();

    closeMenu();

    el = hitTest(x, y);
    if (!el || isInside(el)) return;

    openMenu(el, x, y);
  }

  /* Any press while the menu is open belongs to the menu: run the row, or
   * dismiss on an outside click. Swallowed so the page under the menu does not
   * also react (links would otherwise navigate). */
  function onMouseDown(ev) {
    var x;
    var y;
    var row;

    if (!ev || !st.root || !st.menuOpen) return;

    x = typeof ev.clientX === 'number' ? ev.clientX : st.cursorX;
    y = typeof ev.clientY === 'number' ? ev.clientY : st.cursorY;
    row = rowAt(x, y);

    if (ev.preventDefault) ev.preventDefault();
    if (ev.stopPropagation) ev.stopPropagation();

    if (row >= 0) runMenuAction(row);
    else closeMenu();
  }

  function moveActive(delta) {
    var count = st.labels.length;
    var next;

    if (!count) return;

    if (st.active < 0) next = delta > 0 ? 0 : count - 1;
    else next = (st.active + delta + count) % count;

    setActiveRow(next);
  }

  function onKeyDown(ev) {
    var key = ev ? String(ev.key || '') : '';
    var code = ev ? ev.keyCode : 0;

    if (key === 'Escape' || code === 27) {
      if (st.menuOpen) {
        closeMenu();
        if (ev && ev.preventDefault) ev.preventDefault();
        return;
      }
      if (W[KEY]) W[KEY].destroy();
      return;
    }

    if (!st.menuOpen) return;

    if (key === 'ArrowDown' || code === 40) {
      moveActive(1);
    } else if (key === 'ArrowUp' || code === 38) {
      moveActive(-1);
    } else if (key === 'Enter' || code === 13) {
      if (st.active >= 0) runMenuAction(st.active);
    } else {
      return;
    }

    if (ev.preventDefault) ev.preventDefault();
    if (ev.stopPropagation) ev.stopPropagation();
  }

  function destroy() {
    try { D.removeEventListener('mousemove', onMouseMove, true); } catch (e) {}
    try { D.removeEventListener('keydown', onKeyDown, true); } catch (e) {}
    try { D.removeEventListener('contextmenu', onContextMenu, true); } catch (e) {}
    try { D.removeEventListener('mousedown', onMouseDown, true); } catch (e) {}
    if (st.toastTimer) {
      try { W.clearTimeout(st.toastTimer); } catch (e) {}
      st.toastTimer = 0;
    }
    st.menuOpen = false;
    if (st.root && st.root.parentNode) st.root.parentNode.removeChild(st.root);
    st.root = null;
    st.box = null;
    st.tip = null;
    st.menu = null;
    st.rows = [];
    st.toast = null;
    st.element = null;
    st.pinned = null;
    W[KEY] = null;
  }

  build();

  W[KEY] = {
    version: 2,
    /* Headless/automation hook: client coordinates, same as a mousemove. */
    inspectAt: function (x, y) {
      update(Number(x) || 0, Number(y) || 0);
      return st.element;
    },
    hide: hide,
    destroy: destroy,
    element: function () { return st.element; },
    root: function () { return st.root; },

    /* Headless/automation hook for the context menu: opens it on the element
     * at (x, y) and returns the item labels, or null when nothing is there. */
    menuAt: function (x, y) {
      var cx = Number(x) || 0;
      var cy = Number(y) || 0;
      var el = hitTest(cx, cy);

      closeMenu();
      if (!el || isInside(el)) return null;
      return openMenu(el, cx, cy);
    },
    menuLabels: function () { return st.labels.slice(0); },
    menuSelect: function (label) {
      for (var i = 0; i < st.labels.length; ++i) {
        if (st.labels[i] === label) {
          runMenuAction(i);
          return true;
        }
      }
      return false;
    },
    menuOpen: function () { return st.menuOpen; },
    pinnedElement: function () { return st.pinned; },
    /* The JSON "Copy element info" would put on the clipboard, for the element
     * currently under the cursor (or the one the menu is open on). */
    elementInfo: function () {
      var el = st.pinned || st.element;

      return el ? elementInfo(el) : null;
    }
  };

  D.addEventListener('mousemove', onMouseMove, true);
  D.addEventListener('keydown', onKeyDown, true);
  D.addEventListener('contextmenu', onContextMenu, true);
  D.addEventListener('mousedown', onMouseDown, true);

  if (W.console && W.console.log) {
    W.console.log('[inspector] hover to inspect, right-click for the menu, Esc to remove');
  }
}());
