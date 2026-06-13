/****************************************************************************
** Meta object code from reading C++ file 'layer-stack-widget.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.8.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../src/layers/layer-stack-widget.h"
#include <QtGui/qtextcursor.h>
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'layer-stack-widget.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 68
#error "This file was generated using the moc from 6.8.3. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

#ifndef Q_CONSTINIT
#define Q_CONSTINIT
#endif

QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
QT_WARNING_DISABLE_GCC("-Wuseless-cast")
namespace {
struct qt_meta_tag_ZN10LayerStackE_t {};
} // unnamed namespace


#ifdef QT_MOC_HAS_STRINGDATA
static constexpr auto qt_meta_stringdata_ZN10LayerStackE = QtMocHelpers::stringData(
    "LayerStack",
    "layer_selected",
    "",
    "std::string",
    "layer_id",
    "layers_selected",
    "std::vector<std::string>",
    "layer_ids",
    "layer_visibility_changed",
    "v",
    "layer_lock_changed",
    "locked",
    "layer_expand_changed",
    "expanded",
    "layer_parent_changed",
    "parent_id",
    "layer_mask_changed",
    "mask_source_id",
    "MaskMode",
    "mask_mode",
    "layer_name_changed",
    "name",
    "layer_order_changed",
    "add_layer_requested",
    "LayerType",
    "type",
    "clone_layer_requested",
    "copy_layer_requested",
    "paste_layer_requested",
    "delete_layer_requested",
    "on_add_text",
    "on_add_clock",
    "on_add_ticker",
    "on_add_rect",
    "on_add_image",
    "on_move_up",
    "on_move_down",
    "on_delete",
    "on_item_changed",
    "QListWidgetItem*",
    "item",
    "on_selection_changed",
    "show_layer_context_menu",
    "pos"
);
#else  // !QT_MOC_HAS_STRINGDATA
#error "qtmochelpers.h not found or too old."
#endif // !QT_MOC_HAS_STRINGDATA

Q_CONSTINIT static const uint qt_meta_data_ZN10LayerStackE[] = {

 // content:
      12,       // revision
       0,       // classname
       0,    0, // classinfo
      25,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      14,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    1,  164,    2, 0x06,    1 /* Public */,
       5,    1,  167,    2, 0x06,    3 /* Public */,
       8,    2,  170,    2, 0x06,    5 /* Public */,
      10,    2,  175,    2, 0x06,    8 /* Public */,
      12,    2,  180,    2, 0x06,   11 /* Public */,
      14,    2,  185,    2, 0x06,   14 /* Public */,
      16,    3,  190,    2, 0x06,   17 /* Public */,
      20,    2,  197,    2, 0x06,   21 /* Public */,
      22,    0,  202,    2, 0x06,   24 /* Public */,
      23,    1,  203,    2, 0x06,   25 /* Public */,
      26,    1,  206,    2, 0x06,   27 /* Public */,
      27,    1,  209,    2, 0x06,   29 /* Public */,
      28,    1,  212,    2, 0x06,   31 /* Public */,
      29,    1,  215,    2, 0x06,   33 /* Public */,

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
      30,    0,  218,    2, 0x08,   35 /* Private */,
      31,    0,  219,    2, 0x08,   36 /* Private */,
      32,    0,  220,    2, 0x08,   37 /* Private */,
      33,    0,  221,    2, 0x08,   38 /* Private */,
      34,    0,  222,    2, 0x08,   39 /* Private */,
      35,    0,  223,    2, 0x08,   40 /* Private */,
      36,    0,  224,    2, 0x08,   41 /* Private */,
      37,    0,  225,    2, 0x08,   42 /* Private */,
      38,    1,  226,    2, 0x08,   43 /* Private */,
      41,    0,  229,    2, 0x08,   45 /* Private */,
      42,    1,  230,    2, 0x08,   46 /* Private */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, 0x80000000 | 6,    7,
    QMetaType::Void, 0x80000000 | 3, QMetaType::Bool,    4,    9,
    QMetaType::Void, 0x80000000 | 3, QMetaType::Bool,    4,   11,
    QMetaType::Void, 0x80000000 | 3, QMetaType::Bool,    4,   13,
    QMetaType::Void, 0x80000000 | 3, 0x80000000 | 3,    4,   15,
    QMetaType::Void, 0x80000000 | 3, 0x80000000 | 3, 0x80000000 | 18,    4,   17,   19,
    QMetaType::Void, 0x80000000 | 3, 0x80000000 | 3,    4,   21,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 24,   25,
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, 0x80000000 | 3,    4,
    QMetaType::Void, 0x80000000 | 3,    4,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 39,   40,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QPoint,   43,

       0        // eod
};

Q_CONSTINIT const QMetaObject LayerStack::staticMetaObject = { {
    QMetaObject::SuperData::link<QWidget::staticMetaObject>(),
    qt_meta_stringdata_ZN10LayerStackE.offsetsAndSizes,
    qt_meta_data_ZN10LayerStackE,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_tag_ZN10LayerStackE_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<LayerStack, std::true_type>,
        // method 'layer_selected'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        // method 'layers_selected'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::vector<std::string> &, std::false_type>,
        // method 'layer_visibility_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'layer_lock_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'layer_expand_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'layer_parent_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        // method 'layer_mask_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        QtPrivate::TypeAndForceComplete<MaskMode, std::false_type>,
        // method 'layer_name_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        // method 'layer_order_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'add_layer_requested'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<LayerType, std::false_type>,
        // method 'clone_layer_requested'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        // method 'copy_layer_requested'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        // method 'paste_layer_requested'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        // method 'delete_layer_requested'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        // method 'on_add_text'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_add_clock'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_add_ticker'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_add_rect'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_add_image'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_move_up'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_move_down'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_delete'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_item_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<QListWidgetItem *, std::false_type>,
        // method 'on_selection_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'show_layer_context_menu'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const QPoint &, std::false_type>
    >,
    nullptr
} };

void LayerStack::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<LayerStack *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->layer_selected((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1]))); break;
        case 1: _t->layers_selected((*reinterpret_cast< std::add_pointer_t<std::vector<std::string>>>(_a[1]))); break;
        case 2: _t->layer_visibility_changed((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[2]))); break;
        case 3: _t->layer_lock_changed((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[2]))); break;
        case 4: _t->layer_expand_changed((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<bool>>(_a[2]))); break;
        case 5: _t->layer_parent_changed((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<std::string>>(_a[2]))); break;
        case 6: _t->layer_mask_changed((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<std::string>>(_a[2])),(*reinterpret_cast< std::add_pointer_t<MaskMode>>(_a[3]))); break;
        case 7: _t->layer_name_changed((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1])),(*reinterpret_cast< std::add_pointer_t<std::string>>(_a[2]))); break;
        case 8: _t->layer_order_changed(); break;
        case 9: _t->add_layer_requested((*reinterpret_cast< std::add_pointer_t<LayerType>>(_a[1]))); break;
        case 10: _t->clone_layer_requested((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1]))); break;
        case 11: _t->copy_layer_requested((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1]))); break;
        case 12: _t->paste_layer_requested((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1]))); break;
        case 13: _t->delete_layer_requested((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1]))); break;
        case 14: _t->on_add_text(); break;
        case 15: _t->on_add_clock(); break;
        case 16: _t->on_add_ticker(); break;
        case 17: _t->on_add_rect(); break;
        case 18: _t->on_add_image(); break;
        case 19: _t->on_move_up(); break;
        case 20: _t->on_move_down(); break;
        case 21: _t->on_delete(); break;
        case 22: _t->on_item_changed((*reinterpret_cast< std::add_pointer_t<QListWidgetItem*>>(_a[1]))); break;
        case 23: _t->on_selection_changed(); break;
        case 24: _t->show_layer_context_menu((*reinterpret_cast< std::add_pointer_t<QPoint>>(_a[1]))); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _q_method_type = void (LayerStack::*)(const std::string & );
            if (_q_method_type _q_method = &LayerStack::layer_selected; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(const std::vector<std::string> & );
            if (_q_method_type _q_method = &LayerStack::layers_selected; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 1;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(const std::string & , bool );
            if (_q_method_type _q_method = &LayerStack::layer_visibility_changed; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 2;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(const std::string & , bool );
            if (_q_method_type _q_method = &LayerStack::layer_lock_changed; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 3;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(const std::string & , bool );
            if (_q_method_type _q_method = &LayerStack::layer_expand_changed; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 4;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(const std::string & , const std::string & );
            if (_q_method_type _q_method = &LayerStack::layer_parent_changed; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 5;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(const std::string & , const std::string & , MaskMode );
            if (_q_method_type _q_method = &LayerStack::layer_mask_changed; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 6;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(const std::string & , const std::string & );
            if (_q_method_type _q_method = &LayerStack::layer_name_changed; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 7;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)();
            if (_q_method_type _q_method = &LayerStack::layer_order_changed; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 8;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(LayerType );
            if (_q_method_type _q_method = &LayerStack::add_layer_requested; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 9;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(const std::string & );
            if (_q_method_type _q_method = &LayerStack::clone_layer_requested; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 10;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(const std::string & );
            if (_q_method_type _q_method = &LayerStack::copy_layer_requested; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 11;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(const std::string & );
            if (_q_method_type _q_method = &LayerStack::paste_layer_requested; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 12;
                return;
            }
        }
        {
            using _q_method_type = void (LayerStack::*)(const std::string & );
            if (_q_method_type _q_method = &LayerStack::delete_layer_requested; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 13;
                return;
            }
        }
    }
}

const QMetaObject *LayerStack::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *LayerStack::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ZN10LayerStackE.stringdata0))
        return static_cast<void*>(this);
    return QWidget::qt_metacast(_clname);
}

int LayerStack::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QWidget::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 25)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 25;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 25)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 25;
    }
    return _id;
}

// SIGNAL 0
void LayerStack::layer_selected(const std::string & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void LayerStack::layers_selected(const std::vector<std::string> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void LayerStack::layer_visibility_changed(const std::string & _t1, bool _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void LayerStack::layer_lock_changed(const std::string & _t1, bool _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void LayerStack::layer_expand_changed(const std::string & _t1, bool _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void LayerStack::layer_parent_changed(const std::string & _t1, const std::string & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 5, _a);
}

// SIGNAL 6
void LayerStack::layer_mask_changed(const std::string & _t1, const std::string & _t2, MaskMode _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void LayerStack::layer_name_changed(const std::string & _t1, const std::string & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}

// SIGNAL 8
void LayerStack::layer_order_changed()
{
    QMetaObject::activate(this, &staticMetaObject, 8, nullptr);
}

// SIGNAL 9
void LayerStack::add_layer_requested(LayerType _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 9, _a);
}

// SIGNAL 10
void LayerStack::clone_layer_requested(const std::string & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 10, _a);
}

// SIGNAL 11
void LayerStack::copy_layer_requested(const std::string & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 11, _a);
}

// SIGNAL 12
void LayerStack::paste_layer_requested(const std::string & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 12, _a);
}

// SIGNAL 13
void LayerStack::delete_layer_requested(const std::string & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 13, _a);
}
QT_WARNING_POP
