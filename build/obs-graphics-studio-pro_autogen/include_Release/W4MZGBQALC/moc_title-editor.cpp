/****************************************************************************
** Meta object code from reading C++ file 'title-editor.h'
**
** Created by: The Qt Meta Object Compiler version 68 (Qt 6.8.3)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include "../../../../src/editor/title-editor.h"
#include <QtGui/qtextcursor.h>
#include <QtCore/qmetatype.h>

#include <QtCore/qtmochelpers.h>

#include <memory>


#include <QtCore/qxptype_traits.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'title-editor.h' doesn't include <QObject>."
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
struct qt_meta_tag_ZN11TitleEditorE_t {};
} // unnamed namespace


#ifdef QT_MOC_HAS_STRINGDATA
static constexpr auto qt_meta_stringdata_ZN11TitleEditorE = QtMocHelpers::stringData(
    "TitleEditor",
    "title_saved",
    "",
    "std::string",
    "title_id",
    "play_pause",
    "play_full_loop",
    "rewind",
    "step_forward",
    "previous_keyframe",
    "next_keyframe",
    "on_layer_selected",
    "layer_id",
    "on_playhead_changed",
    "t",
    "on_title_modified",
    "push_undo_snapshot",
    "tick",
    "show_about",
    "show_preferences",
    "reject"
);
#else  // !QT_MOC_HAS_STRINGDATA
#error "qtmochelpers.h not found or too old."
#endif // !QT_MOC_HAS_STRINGDATA

Q_CONSTINIT static const uint qt_meta_data_ZN11TitleEditorE[] = {

 // content:
      12,       // revision
       0,       // classname
       0,    0, // classinfo
      15,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       1,       // signalCount

 // signals: name, argc, parameters, tag, flags, initial metatype offsets
       1,    1,  104,    2, 0x06,    1 /* Public */,

 // slots: name, argc, parameters, tag, flags, initial metatype offsets
       5,    0,  107,    2, 0x0a,    3 /* Public */,
       6,    0,  108,    2, 0x0a,    4 /* Public */,
       7,    0,  109,    2, 0x0a,    5 /* Public */,
       8,    0,  110,    2, 0x0a,    6 /* Public */,
       9,    0,  111,    2, 0x0a,    7 /* Public */,
      10,    0,  112,    2, 0x0a,    8 /* Public */,
      11,    1,  113,    2, 0x0a,    9 /* Public */,
      13,    1,  116,    2, 0x0a,   11 /* Public */,
      15,    1,  119,    2, 0x0a,   13 /* Public */,
      15,    0,  122,    2, 0x2a,   15 /* Public | MethodCloned */,
      17,    0,  123,    2, 0x08,   16 /* Private */,
      18,    0,  124,    2, 0x08,   17 /* Private */,
      19,    0,  125,    2, 0x08,   18 /* Private */,
      20,    0,  126,    2, 0x08,   19 /* Private */,

 // signals: parameters
    QMetaType::Void, 0x80000000 | 3,    4,

 // slots: parameters
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, 0x80000000 | 3,   12,
    QMetaType::Void, QMetaType::Double,   14,
    QMetaType::Void, QMetaType::Bool,   16,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

Q_CONSTINIT const QMetaObject TitleEditor::staticMetaObject = { {
    QMetaObject::SuperData::link<QMainWindow::staticMetaObject>(),
    qt_meta_stringdata_ZN11TitleEditorE.offsetsAndSizes,
    qt_meta_data_ZN11TitleEditorE,
    qt_static_metacall,
    nullptr,
    qt_incomplete_metaTypeArray<qt_meta_tag_ZN11TitleEditorE_t,
        // Q_OBJECT / Q_GADGET
        QtPrivate::TypeAndForceComplete<TitleEditor, std::true_type>,
        // method 'title_saved'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        // method 'play_pause'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'play_full_loop'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'rewind'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'step_forward'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'previous_keyframe'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'next_keyframe'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'on_layer_selected'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<const std::string &, std::false_type>,
        // method 'on_playhead_changed'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<double, std::false_type>,
        // method 'on_title_modified'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        QtPrivate::TypeAndForceComplete<bool, std::false_type>,
        // method 'on_title_modified'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'tick'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'show_about'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'show_preferences'
        QtPrivate::TypeAndForceComplete<void, std::false_type>,
        // method 'reject'
        QtPrivate::TypeAndForceComplete<void, std::false_type>
    >,
    nullptr
} };

void TitleEditor::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    auto *_t = static_cast<TitleEditor *>(_o);
    if (_c == QMetaObject::InvokeMetaMethod) {
        switch (_id) {
        case 0: _t->title_saved((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1]))); break;
        case 1: _t->play_pause(); break;
        case 2: _t->play_full_loop(); break;
        case 3: _t->rewind(); break;
        case 4: _t->step_forward(); break;
        case 5: _t->previous_keyframe(); break;
        case 6: _t->next_keyframe(); break;
        case 7: _t->on_layer_selected((*reinterpret_cast< std::add_pointer_t<std::string>>(_a[1]))); break;
        case 8: _t->on_playhead_changed((*reinterpret_cast< std::add_pointer_t<double>>(_a[1]))); break;
        case 9: _t->on_title_modified((*reinterpret_cast< std::add_pointer_t<bool>>(_a[1]))); break;
        case 10: _t->on_title_modified(); break;
        case 11: _t->tick(); break;
        case 12: _t->show_about(); break;
        case 13: _t->show_preferences(); break;
        case 14: _t->reject(); break;
        default: ;
        }
    }
    if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _q_method_type = void (TitleEditor::*)(const std::string & );
            if (_q_method_type _q_method = &TitleEditor::title_saved; *reinterpret_cast<_q_method_type *>(_a[1]) == _q_method) {
                *result = 0;
                return;
            }
        }
    }
}

const QMetaObject *TitleEditor::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *TitleEditor::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ZN11TitleEditorE.stringdata0))
        return static_cast<void*>(this);
    return QMainWindow::qt_metacast(_clname);
}

int TitleEditor::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QMainWindow::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 15)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 15;
    }
    if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 15)
            *reinterpret_cast<QMetaType *>(_a[0]) = QMetaType();
        _id -= 15;
    }
    return _id;
}

// SIGNAL 0
void TitleEditor::title_saved(const std::string & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}
QT_WARNING_POP
