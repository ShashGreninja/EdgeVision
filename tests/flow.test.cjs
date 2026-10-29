const { test } = require('node:test');
const assert = require('node:assert/strict');
const flow = require('../dashboard/flow.js');

test('navigation preserves configured API and module without copying unrelated parameters', () => {
  const url = new URL(flow.destination('app.html', '?api=https%3A%2F%2Fexample.com%2Fevents&module=http%3A%2F%2Flocalhost%3A8081&token=secret', 'live'), 'http://localhost/');
  assert.equal(url.searchParams.get('api'), 'https://example.com/events');
  assert.equal(url.searchParams.get('module'), 'http://localhost:8081');
  assert.equal(url.searchParams.get('mode'), 'live');
  assert.equal(url.searchParams.has('token'), false);
});
test('the guide and home keep the selected demo mode', () => {
  assert.equal(new URL(flow.destination('guide.html', '?mode=live'), 'http://localhost/').searchParams.get('mode'), 'live');
});
test('a disconnected stream never reports a running camera', () => {
  assert.equal(flow.journey({ connected: false, telemetry: { state: 'streaming' } }).stage, 'connect');
});
test('old cloud history cannot complete a newly triggered visit', () => {
  assert.equal(flow.journey({ connected: true, triggeredAt: 2000, events: [{ received_at: 1000 }], telemetry: { state: 'streaming', network: { queued: 0 } } }).stage, 'detect');
});
test('another device cannot complete the current camera journey', () => {
  assert.equal(flow.journey({ connected: true, triggeredAt: 2000, events: [{ received_at: 2100, device: 'other' }], telemetry: { device: 'edge-cam-01' } }).stage, 'detect');
});
test('a self-test cannot masquerade as a new camera detection', () => {
  assert.equal(flow.journey({ connected: true, triggeredAt: 2000, expectedEvent: 'detection', events: [{ received_at: 2100, event: 'selftest' }], telemetry: {} }).stage, 'detect');
});
test('a new stored event completes the journey and an offline queue is explicit', () => {
  assert.equal(flow.journey({ connected: true, triggeredAt: 2000, events: [{ received_at: 2100 }], telemetry: {} }).stage, 'stored');
  assert.equal(flow.journey({ connected: true, triggeredAt: 2000, events: [], telemetry: { network: { queued: 2, online: false } } }).stage, 'queued');
});
test('payload saving is unknown before a baseline exists and never rounded up to 100%', () => {
  assert.equal(flow.savings(0, 0), null);
  assert.equal(flow.savings(173, 8500000), '99.99');
});
test('API query parameters are merged without losing the chosen device', () => {
  const url = new URL(flow.eventsURL('https://example.com/events?device=door-2', 10));
  assert.equal(url.searchParams.get('device'), 'door-2');
  assert.equal(url.searchParams.get('limit'), '10');
});
test('cloud data becomes text, never markup, and filters agree with the visible rows', () => {
  const events = [{ event: 'person_detected' }, { event: 'selftest' }, { event: 'animal_detected' }];
  assert.equal(flow.filterEvents(events, 'detections').length, 2);
  assert.equal(flow.filterEvents(events, 'selftest').length, 1);
  assert.equal(flow.eventLabel('<img onerror=alert(1)>'), '<img onerror=alert(1)>');
});
