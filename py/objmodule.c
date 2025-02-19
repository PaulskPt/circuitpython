/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2013-2019 Damien P. George
 * Copyright (c) 2014-2015 Paul Sokolovsky
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdlib.h>
#include <string.h>
#include <assert.h>

#include "py/bc.h"
#include "py/objmodule.h"
#include "py/runtime.h"
#include "py/builtin.h"

#ifndef NO_QSTR
// Only include module definitions when not doing qstr extraction, because the
// qstr extraction stage also generates this module definition header file.
#include "genhdr/moduledefs.h"
#endif

#if MICROPY_MODULE_BUILTIN_INIT
STATIC void mp_module_call_init(mp_obj_t module_name, mp_obj_t module_obj);
#endif

STATIC void module_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    mp_obj_module_t *self = MP_OBJ_TO_PTR(self_in);

    const char *module_name = "";
    mp_map_elem_t *elem = mp_map_lookup(&self->globals->map, MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_MAP_LOOKUP);
    if (elem != NULL) {
        module_name = mp_obj_str_get_str(elem->value);
    }

    #if MICROPY_PY___FILE__
    // If we store __file__ to imported modules then try to lookup this
    // symbol to give more information about the module.
    elem = mp_map_lookup(&self->globals->map, MP_OBJ_NEW_QSTR(MP_QSTR___file__), MP_MAP_LOOKUP);
    if (elem != NULL) {
        mp_printf(print, "<module '%s' from '%s'>", module_name, mp_obj_str_get_str(elem->value));
        return;
    }
    #endif

    mp_printf(print, "<module '%s'>", module_name);
}

STATIC void module_attr_try_delegation(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    #if MICROPY_MODULE_ATTR_DELEGATION
    // Delegate lookup to a module's custom attr method (found in last lot of globals dict).
    mp_obj_module_t *self = MP_OBJ_TO_PTR(self_in);
    mp_map_t *map = &self->globals->map;
    if (map->table[map->alloc - 1].key == MP_OBJ_NEW_QSTR(MP_QSTRnull)) {
        ((mp_attr_fun_t)MP_OBJ_TO_PTR(map->table[map->alloc - 1].value))(self_in, attr, dest);
    }
    #else
    (void)self_in;
    (void)attr;
    (void)dest;
    #endif
}

STATIC void module_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    mp_obj_module_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        // load attribute
        mp_map_elem_t *elem = mp_map_lookup(&self->globals->map, MP_OBJ_NEW_QSTR(attr), MP_MAP_LOOKUP);
        if (elem != NULL) {
            dest[0] = elem->value;
        #if MICROPY_MODULE_GETATTR
        } else if (attr != MP_QSTR___getattr__) {
            elem = mp_map_lookup(&self->globals->map, MP_OBJ_NEW_QSTR(MP_QSTR___getattr__), MP_MAP_LOOKUP);
            if (elem != NULL) {
                dest[0] = mp_call_function_1(elem->value, MP_OBJ_NEW_QSTR(attr));
            } else {
                module_attr_try_delegation(self_in, attr, dest);
            }
        #endif
        } else {
            module_attr_try_delegation(self_in, attr, dest);
        }
    } else {
        // delete/store attribute
        mp_obj_dict_t *dict = self->globals;
        if (dict->map.is_fixed) {
            #if MICROPY_CAN_OVERRIDE_BUILTINS
            if (dict == &mp_module_builtins_globals) {
                if (MP_STATE_VM(mp_module_builtins_override_dict) == NULL) {
                    MP_STATE_VM(mp_module_builtins_override_dict) = MP_OBJ_TO_PTR(mp_obj_new_dict(1));
                }
                dict = MP_STATE_VM(mp_module_builtins_override_dict);
            } else
            #endif
            {
                // can't delete or store to fixed map
                module_attr_try_delegation(self_in, attr, dest);
                return;
            }
        }
        if (dest[1] == MP_OBJ_NULL) {
            // delete attribute
            mp_obj_dict_delete(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(attr));
        } else {
            // store attribute
            mp_obj_dict_store(MP_OBJ_FROM_PTR(dict), MP_OBJ_NEW_QSTR(attr), dest[1]);
        }
        dest[0] = MP_OBJ_NULL; // indicate success
    }
}

const mp_obj_type_t mp_type_module = {
    { &mp_type_type },
    .name = MP_QSTR_module,
    .print = module_print,
    .attr = module_attr,
};

mp_obj_t mp_obj_new_module(qstr module_name) {
    mp_map_t *mp_loaded_modules_map = &MP_STATE_VM(mp_loaded_modules_dict).map;
    mp_map_elem_t *el = mp_map_lookup(mp_loaded_modules_map, MP_OBJ_NEW_QSTR(module_name), MP_MAP_LOOKUP_ADD_IF_NOT_FOUND);
    // We could error out if module already exists, but let C extensions
    // add new members to existing modules.
    if (el->value != MP_OBJ_NULL) {
        return el->value;
    }

    // create new module object
    mp_module_context_t *o = m_new_obj(mp_module_context_t);
    o->module.base.type = &mp_type_module;
    o->module.globals = MP_OBJ_TO_PTR(mp_obj_new_dict(MICROPY_MODULE_DICT_SIZE));

    // store __name__ entry in the module
    mp_obj_dict_store(MP_OBJ_FROM_PTR(o->module.globals), MP_OBJ_NEW_QSTR(MP_QSTR___name__), MP_OBJ_NEW_QSTR(module_name));

    // store the new module into the slot in the global dict holding all modules
    el->value = MP_OBJ_FROM_PTR(o);

    // return the new module
    return MP_OBJ_FROM_PTR(o);
}

mp_obj_dict_t *mp_obj_module_get_globals(mp_obj_t self_in) {
    assert(mp_obj_is_type(self_in, &mp_type_module));
    mp_obj_module_t *self = MP_OBJ_TO_PTR(self_in);
    return self->globals;
}

void mp_obj_module_set_globals(mp_obj_t self_in, mp_obj_dict_t *globals) {
    assert(mp_obj_is_type(self_in, &mp_type_module));
    mp_obj_module_t *self = MP_OBJ_TO_PTR(self_in);
    self->globals = globals;
}

/******************************************************************************/
// Global module table and related functions

STATIC const mp_rom_map_elem_t mp_builtin_module_table[] = {
    // builtin modules declared with MP_REGISTER_MODULE()
    MICROPY_REGISTERED_MODULES
};

MP_DEFINE_CONST_MAP(mp_builtin_module_map, mp_builtin_module_table);

// Tries to find a loaded module, otherwise attempts to load a builtin, otherwise MP_OBJ_NULL.
mp_obj_t mp_module_get_loaded_or_builtin(qstr module_name) {
    // First try loaded modules.
    mp_map_elem_t *elem = mp_map_lookup(&MP_STATE_VM(mp_loaded_modules_dict).map, MP_OBJ_NEW_QSTR(module_name), MP_MAP_LOOKUP);

    if (!elem) {
        #if MICROPY_MODULE_WEAK_LINKS
        return mp_module_get_builtin(module_name);
        #else
        // Otherwise try builtin.
        elem = mp_map_lookup((mp_map_t *)&mp_builtin_module_map, MP_OBJ_NEW_QSTR(module_name), MP_MAP_LOOKUP);
        if (!elem) {
            return MP_OBJ_NULL;
        }

        #if MICROPY_MODULE_BUILTIN_INIT
        // If found, it's a newly loaded built-in, so init it.
        mp_module_call_init(MP_OBJ_NEW_QSTR(module_name), elem->value);
        #endif
        #endif
    }

    return elem->value;
}

#if MICROPY_MODULE_WEAK_LINKS
// Tries to find a loaded module, otherwise attempts to load a builtin, otherwise MP_OBJ_NULL.
mp_obj_t mp_module_get_builtin(qstr module_name) {
    // Try builtin.
    mp_map_elem_t *elem = mp_map_lookup((mp_map_t *)&mp_builtin_module_map, MP_OBJ_NEW_QSTR(module_name), MP_MAP_LOOKUP);
    if (!elem) {
        return MP_OBJ_NULL;
    }

    #if MICROPY_MODULE_BUILTIN_INIT
    // If found, it's a newly loaded built-in, so init it.
    mp_module_call_init(MP_OBJ_NEW_QSTR(module_name), elem->value);
    #endif

    return elem->value;
}
#endif

#if MICROPY_MODULE_BUILTIN_INIT
STATIC void mp_module_register(mp_obj_t module_name, mp_obj_t module) {
    mp_map_t *mp_loaded_modules_map = &MP_STATE_VM(mp_loaded_modules_dict).map;
    mp_map_lookup(mp_loaded_modules_map, module_name, MP_MAP_LOOKUP_ADD_IF_NOT_FOUND)->value = module;
}

STATIC void mp_module_call_init(mp_obj_t module_name, mp_obj_t module_obj) {
    // Look for __init__ and call it if it exists
    mp_obj_t dest[2];
    mp_load_method_maybe(module_obj, MP_QSTR___init__, dest);
    if (dest[0] != MP_OBJ_NULL) {
        mp_call_method_n_kw(0, 0, dest);
        // Register module so __init__ is not called again.
        // If a module can be referenced by more than one name (eg due to weak links)
        // then __init__ will still be called for each distinct import, and it's then
        // up to the particular module to make sure it's __init__ code only runs once.
        mp_module_register(module_name, module_obj);
    }
}
#endif

void mp_module_generic_attr(qstr attr, mp_obj_t *dest, const uint16_t *keys, mp_obj_t *values) {
    for (size_t i = 0; keys[i] != MP_QSTRnull; ++i) {
        if (attr == keys[i]) {
            if (dest[0] == MP_OBJ_NULL) {
                // load attribute (MP_OBJ_NULL returned for deleted items)
                dest[0] = values[i];
            } else {
                // delete or store (delete stores MP_OBJ_NULL)
                values[i] = dest[1];
                dest[0] = MP_OBJ_NULL; // indicate success
            }
            return;
        }
    }
}
