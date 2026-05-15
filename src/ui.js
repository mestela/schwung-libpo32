import { createEnum, createValue, createAction, createSubmenu, createBack } from '/data/UserData/schwung/shared/menu_items.mjs';
import { createMenuState, handleMenuInput } from '/data/UserData/schwung/shared/menu_nav.mjs';
import { createMenuStack } from '/data/UserData/schwung/shared/menu_stack.mjs';
import { drawMenuList, drawMenuHeader, drawMenuFooter, menuLayoutDefaults } from '/data/UserData/schwung/shared/menu_layout.mjs';

const CC_JOG_WHEEL = 14;

let menuState;
let menuStack;
let needsRedraw = true;
let kitNames = [];
let kitOptions = [];

function loadKitNames() {
    const allNames = host_module_get_param('kit_names_all') || '';
    const cnt = host_module_get_param('kit_count') || '0';
    console.log('po32 load: count=' + cnt + ' names=[' + allNames + ']');
    kitNames = allNames ? allNames.split('|') : [];
    // Mutate in-place so enum items that captured this array reference stay in sync
    kitOptions.splice(0, kitOptions.length, ...kitNames.map((_, i) => String(i)));
}

function padLabel(i) {
    return 'P' + String(i + 1).padStart(2, '0');
}

function makeInstParams(instIdx) {
    host_module_set_param('inst', String(instIdx));
    const waves = ['Sine', 'Tri', 'Saw'];
    const mods  = ['Drop', 'Sine', 'Noise'];
    return [
        createBack(),
        createEnum('Wave', {
            get: () => host_module_get_param('inst_wave') || '0',
            set: (v) => { host_module_set_param('inst', String(instIdx)); host_module_set_param('inst_wave', v); },
            options: ['0', '1', '2'],
            format: (v) => waves[parseInt(v)] || v
        }),
        createValue('Pitch', {
            get: () => parseInt(host_module_get_param('inst_freq') || '50'),
            set: (v) => { host_module_set_param('inst', String(instIdx)); host_module_set_param('inst_freq', String(v)); },
            min: 0, max: 100, step: 2
        }),
        createValue('Decay', {
            get: () => parseInt(host_module_get_param('inst_dcy') || '50'),
            set: (v) => { host_module_set_param('inst', String(instIdx)); host_module_set_param('inst_dcy', String(v)); },
            min: 0, max: 100, step: 2
        }),
        createEnum('Mod', {
            get: () => host_module_get_param('inst_mod_mode') || '0',
            set: (v) => { host_module_set_param('inst', String(instIdx)); host_module_set_param('inst_mod_mode', v); },
            options: ['0', '1', '2'],
            format: (v) => mods[parseInt(v)] || v
        }),
        createValue('Bend', {
            get: () => parseInt(host_module_get_param('inst_mod_amt') || '50'),
            set: (v) => { host_module_set_param('inst', String(instIdx)); host_module_set_param('inst_mod_amt', String(v)); },
            min: 0, max: 100, step: 2
        }),
        createValue('Noise', {
            get: () => parseInt(host_module_get_param('inst_noise') || '50'),
            set: (v) => { host_module_set_param('inst', String(instIdx)); host_module_set_param('inst_noise', String(v)); },
            min: 0, max: 100, step: 2
        }),
        createValue('Dist', {
            get: () => parseInt(host_module_get_param('inst_dist') || '0'),
            set: (v) => { host_module_set_param('inst', String(instIdx)); host_module_set_param('inst_dist', String(v)); },
            min: 0, max: 100, step: 2
        }),
        createValue('Level', {
            get: () => parseInt(host_module_get_param('inst_level') || '80'),
            set: (v) => { host_module_set_param('inst', String(instIdx)); host_module_set_param('inst_level', String(v)); },
            min: 0, max: 100, step: 2
        }),
        createAction('Randomize', () => {
            host_module_set_param('randomize_inst', String(instIdx));
            needsRedraw = true;
        })
    ];
}

globalThis.init = function() {
    loadKitNames();
    menuState = createMenuState();
    menuStack = createMenuStack();

    const soundsMenu = createSubmenu('Sounds', () => [
        createBack(),
        ...Array.from({ length: 16 }, (_, i) =>
            createSubmenu(padLabel(i), () => {
                host_module_set_param('inst', String(i));
                return makeInstParams(i);
            })
        )
    ]);

    const params = [
        createEnum('Kit', {
            get: () => host_module_get_param('kit') || '0',
            set: (v) => host_module_set_param('kit', v),
            options: kitOptions,
            format: (v) => kitNames[parseInt(v)] || v
        }),
        createValue('Decay', {
            get: () => Math.round(parseFloat(host_module_get_param('decay') || '1.0') * 100),
            set: (v) => host_module_set_param('decay', String(v / 100)),
            min: 10, max: 300, step: 5,
            format: (v) => (v / 100).toFixed(2) + 'x'
        }),
        createValue('Level', {
            get: () => Math.round(parseFloat(host_module_get_param('level') || '1.0') * 100),
            set: (v) => host_module_set_param('level', String(v / 100)),
            min: 0, max: 100, step: 5,
            format: (v) => v + '%'
        }),
        soundsMenu,
        createAction('Save Kit', () => {
            host_module_set_param('save_kit', '1');
            loadKitNames();
            needsRedraw = true;
        }),
        createAction('Randomize', () => host_module_set_param('randomize', '1'))
    ];

    menuStack.push({ title: 'libpo32', items: params });
    needsRedraw = true;
};

globalThis.tick = function() {
    if (!needsRedraw) return;
    clear_screen();
    const current = menuStack.current();
    drawMenuHeader(current.title);
    drawMenuList({
        items: current.items,
        selectedIndex: menuState.selectedIndex,
        listArea: { topY: menuLayoutDefaults.listTopY, bottomY: menuLayoutDefaults.listBottomWithFooter },
        valueAlignRight: true,
        valueX: 60,
        valuePaddingRight: 8,
        editMode: menuState.editing,
        getLabel: (item) => item.label,
        getValue: (item, index) => {
            if (item.type === 'action' || item.type === 'submenu') return '';
            const inEdit = menuState.editing && index === menuState.selectedIndex;
            const raw = (inEdit && menuState.editValue !== null)
                ? menuState.editValue
                : (item.get ? item.get() : null);
            if (raw === null) return '';
            if (item.format) return item.format(raw);
            if (item.options) return item.options[parseInt(raw)] || String(raw);
            return String(raw);
        }
    });
    drawMenuFooter('Jog:scroll  Click:select');
    needsRedraw = false;
};

globalThis.onMidiMessageInternal = function(data) {
    const status = data[0] & 0xF0;
    if (status !== 0xB0) return;
    const cc = data[1];
    const current = menuStack.current();
    const result = handleMenuInput({
        cc, value: data[2],
        items: current.items,
        state: menuState,
        stack: menuStack,
        shiftHeld: false,
        onBack: () => {}
    });
    if (menuState.editing && cc === CC_JOG_WHEEL && menuState.editValue !== null) {
        const item = current.items[menuState.selectedIndex];
        if (item && item.set) item.set(menuState.editValue);
    }
    if (result.needsRedraw) needsRedraw = true;
};

globalThis.onMidiMessageExternal = function(data) {};
