(() => {
  'use strict';
  const views = ['csv', 'logs'];
  const tabs = views.map(view => document.getElementById('tab-' + view));
  const tablist = document.getElementById('record-tabs');
  const compact = window.matchMedia('(max-width:820px)');
  let active = null;
  const readView = () => views.includes(location.hash.slice(1)) ? location.hash.slice(1) : 'csv';
  function orientation() { tablist.setAttribute('aria-orientation', compact.matches ? 'horizontal' : 'vertical'); }
  function activate(view, updateHash = false) {
    if (!views.includes(view)) view = 'csv';
    const changed = active !== view;
    if (changed) {
      const dialog = document.getElementById('record-dialog');
      if (dialog.open && view !== 'csv') dialog.close();
    }
    for (let i = 0; i < views.length; i++) {
      const selected = views[i] === view;
      document.getElementById(views[i] + '-view').hidden = !selected;
      document.getElementById(views[i] + '-sources').hidden = !selected;
      tabs[i].classList.toggle('is-active', selected);
      tabs[i].setAttribute('aria-selected', String(selected));
      tabs[i].tabIndex = selected ? 0 : -1;
    }
    active = view;
    if (updateHash && location.hash !== '#' + view) location.hash = view;
    if (changed) {
      if (document.activeElement && document.activeElement.closest('main[hidden]')) tabs[views.indexOf(view)].focus({ preventScroll: true });
      window.scrollTo(0, 0);
      // ResizeObserver redraws visible charts; this also supports the resize-event fallback.
      requestAnimationFrame(() => window.dispatchEvent(new Event('resize')));
    }
  }
  tabs.forEach((tab, index) => {
    tab.addEventListener('click', () => activate(views[index], true));
    tab.addEventListener('keydown', event => {
      let next = index;
      if (event.key === 'Home') next = 0;
      else if (event.key === 'End') next = tabs.length - 1;
      else if (event.key === 'ArrowRight' || event.key === 'ArrowDown') next = (index + 1) % tabs.length;
      else if (event.key === 'ArrowLeft' || event.key === 'ArrowUp') next = (index + tabs.length - 1) % tabs.length;
      else return;
      event.preventDefault();
      activate(views[next], true);
      tabs[next].focus({ preventScroll: true });
    });
  });
  window.addEventListener('hashchange', () => activate(readView()));
  if (compact.addEventListener) compact.addEventListener('change', orientation); else compact.addListener(orientation);
  orientation();
  activate(readView());
})();
