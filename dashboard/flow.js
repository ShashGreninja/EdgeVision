(function (root) {
  'use strict';
  const destination = (path, search = '', mode) => {
    const incoming = new URLSearchParams(search), outgoing = new URLSearchParams();
    for (const key of ['api', 'module']) if (incoming.has(key)) outgoing.set(key, incoming.get(key));
    const selectedMode = mode || incoming.get('mode');
    if (['live', 'recorded'].includes(selectedMode)) outgoing.set('mode', selectedMode);
    return path + (outgoing.size ? '?' + outgoing : '');
  };
  const eventsURL = (api, limit) => {
    const url = new URL(api, typeof location === 'undefined' ? 'http://localhost/' : location.href);
    url.searchParams.set('limit', limit);
    return url.href;
  };
  const savings = (sent, baseline) => baseline > 0 ? (Math.floor(Math.max(0, Math.min(100, (1 - sent / baseline) * 100)) * 100) / 100).toFixed(2) : null;
  const eventLabel = (value) => ({ person_detected: 'Person detected', vehicle_detected: 'Vehicle detected', animal_detected: 'Animal detected', selftest: 'Test event' }[value] || String(value || 'Unknown event').replace(/_detected$/, ' detected').replace(/_/g, ' '));
  const filterEvents = (events, filter) => events.filter(e => filter === 'all' || (filter === 'detections' ? /_detected$/.test(e.event) : e.event === filter));
  const journey = ({ connected, telemetry = {}, triggeredAt, events = [], expectedEvent = 'any' }) => {
    if (!connected) return { stage: 'connect', title: 'Connect your simulator', detail: 'Start the local demo, then check its connection here.' };
    if (!triggeredAt) return { stage: 'ready', title: 'Ready for a visit', detail: 'Trigger motion to wake the camera and follow an event.' };
    if (events.some(e => e.received_at >= triggeredAt && (!telemetry.device || e.device === telemetry.device) && (expectedEvent !== 'detection' || /_detected$/.test(e.event)))) return { stage: 'stored', title: 'Your event reached the cloud', detail: 'The backend returned a new event from this visit.' };
    if (telemetry.network?.queued > 0 && !telemetry.network?.online) return { stage: 'queued', title: 'Held safely on the device', detail: 'The connection is down. Queued events wait for it to return.' };
    return { stage: 'detect', title: 'The camera is checking', detail: 'Useful frames reach the detector. Repeated hits confirm an event.' };
  };
  const api = { destination, eventsURL, savings, eventLabel, filterEvents, journey };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  root.EdgeFlow = api;
})(typeof window === 'undefined' ? globalThis : window);
