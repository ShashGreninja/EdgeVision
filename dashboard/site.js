(() => {
  'use strict';
  document.querySelectorAll('[data-destination]').forEach(link => {
    const mode = link.dataset.mode === 'current' ? (new URLSearchParams(location.search).get('mode') || 'recorded') : link.dataset.mode;
    link.href = EdgeFlow.destination(link.dataset.destination, location.search, mode);
  });
  const frames = ['awake', 'gate', 'detect', 'event'];
  const descriptions = ['The camera sleeps until a motion trigger arrives.', 'A cheap motion check filters quiet frames before expensive inference.', 'Three qualifying hits in five recent checks confirm the detection.', 'Only the event is published. AWS stores it and the dashboard retrieves it.'];
  document.querySelectorAll('[data-walk]').forEach(button => button.addEventListener('click', () => {
    const index = Number(button.dataset.walk);
    document.querySelectorAll('[data-walk]').forEach(b => b.setAttribute('aria-pressed', String(b === button)));
    const preview = document.getElementById('walk-image');
    if (preview) preview.src = `assets/frame-${frames[index]}.jpg`;
    document.getElementById('walk-caption').textContent = descriptions[index];
  }));
  if (matchMedia('(prefers-reduced-motion: reduce)').matches) document.querySelectorAll('video[autoplay]').forEach(video => video.pause());
})();
