const assert = require('node:assert/strict');
const {readFileSync} = require('node:fs');
const {join} = require('node:path');
const {test} = require('node:test');
const vm = require('node:vm');

const source = readFileSync(join(__dirname, '../web/root.js'), 'utf8');
function section(start, end) {
  const from = source.indexOf(start), to = source.indexOf(end, from);
  assert.ok(from >= 0 && to > from, `Missing dashboard section: ${start}`);
  return source.slice(from, to);
}

function dashboard() {
  function element() {
    const classes = new Set(['hidden']);
    return {
      dataset: {}, textContent: '', children: [],
      classList: {add: name => classes.add(name), remove: name => classes.delete(name),
        contains: name => classes.has(name),
        toggle: (name, force) => force ? classes.add(name) : classes.delete(name)},
      replaceChildren() { this.children = []; },
      appendChild(child) { this.children.push(child); },
      setAttribute() {},
    };
  }
  const elements = new Map();
  const get = id => {
    if (!elements.has(id)) elements.set(id, element());
    return elements.get(id);
  };
  const context = vm.createContext({
    $: get, txt: (id, value) => { get(id).textContent = value; },
    document: {createElement: element}, asicPowerBusy: false, asicRestartBusy: false,
    fetch: async () => ({ok: false, status: 500,
      json: async () => ({error: 'ESP_FAIL'})}),
    // Legacy dismissal must not suppress the first status on a new page visit.
    sessionStorage: {getItem: () => 'no BM1370 detected'},
  });
  vm.runInContext(section("let dismissedFault=", "$('risk-cancel')") +
    section('function setFault(', 'function setUnsupportedBoard(') +
    section('function setBoot(', 'function setMetric(') +
    section('async function applyAsicPower(', 'async function setAsicPower(') +
    section('async function swarmDeviceAction(', 'async function refreshSwarm(') +
    section('function setAsicPowerButton(', 'function setOverclock('), context);
  return {
    get,
    status(s) { context.setFault(s); context.setBoot(s); context.setAsicPowerButton(s); },
    visible() { return !get('fault-panel').classList.contains('hidden'); },
    retry() { return context.applyAsicPower(true, true); },
    swarmRetry() { return context.swarmDeviceAction({local: true}, 'asic-on'); },
  };
}

const stopped = {asic_power_enabled: false, asic_ready: false, booting: false,
  hardware_fault: false, power_fault: 0, hardware_fault_msg: 'no BM1370 detected'};

test('failed ASIC startup shows the reason and permits a manual retry', () => {
  const ui = dashboard();
  ui.status(stopped);
  assert.equal(ui.visible(), true);
  assert.equal(ui.get('fault-title').textContent, 'ASIC Stopped');
  assert.equal(ui.get('fault-reasons').children[0].textContent, stopped.hardware_fault_msg);
  assert.equal(ui.get('oc-asic-power').disabled, false);
  assert.equal(ui.get('oc-asic-power').textContent, 'Turn On');
});

test('startup hides the terminal banner, successful startup and manual off stay clear', () => {
  const ui = dashboard();
  ui.status({...stopped, booting: true});
  assert.equal(ui.visible(), false);
  assert.equal(ui.get('oc-asic-power').disabled, true);
  assert.equal(ui.get('boot-panel').classList.contains('hidden'), false);
  ui.status({...stopped, booting: true, hardware_fault_msg: ''});
  assert.equal(ui.get('boot-panel').classList.contains('hidden'), false);
  ui.status({...stopped, asic_power_enabled: true, asic_ready: true, hardware_fault_msg: ''});
  assert.equal(ui.visible(), false);
  ui.status({...stopped, hardware_fault_msg: ''});
  assert.equal(ui.visible(), false);
});

test('safety shutdown remains visible and prevents re-enabling the ASIC', () => {
  const ui = dashboard();
  ui.status({...stopped, hardware_fault: true, power_fault: 1});
  assert.equal(ui.visible(), true);
  assert.equal(ui.get('fault-title').textContent, 'Safety Shutdown');
  assert.equal(ui.get('oc-asic-power').disabled, true);
  ui.status({...stopped, hardware_fault: true, unsupported_board: true});
  assert.equal(ui.visible(), false);
});

test('dismissal lasts for the current occurrence and page visit only', () => {
  const ui = dashboard();
  ui.status(stopped);
  ui.get('fault-close').onclick();
  ui.status(stopped);
  assert.equal(ui.visible(), false);
  ui.status({...stopped, booting: true});
  ui.status(stopped);
  assert.equal(ui.visible(), true);
  const freshVisit = dashboard();
  freshVisit.status(stopped);
  assert.equal(freshVisit.visible(), true);
});

test('diagnostic text is rendered literally', () => {
  const ui = dashboard();
  const reason = 'VIN < 4.8V; <img src=x onerror=alert(1)>';
  ui.status({...stopped, hardware_fault_msg: reason});
  assert.equal(ui.get('fault-reasons').children[0].textContent, reason);
});

test('a manual retry reopens an identical dismissed failure without an interim poll', async () => {
  const ui = dashboard();
  ui.status(stopped);
  ui.get('fault-close').onclick();
  await assert.rejects(ui.retry(), /ESP_FAIL/);
  ui.status(stopped);
  assert.equal(ui.visible(), true);
  ui.get('fault-close').onclick();
  await ui.swarmRetry();
  ui.status(stopped);
  assert.equal(ui.visible(), true);
});
