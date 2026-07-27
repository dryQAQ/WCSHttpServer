/****************************************************************************
** Meta object code from reading C++ file 'WaveManager.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../WaveManager.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'WaveManager.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_WaveManager_t {
    QByteArrayData data[12];
    char stringdata0[133];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_WaveManager_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_WaveManager_t qt_meta_stringdata_WaveManager = {
    {
QT_MOC_LITERAL(0, 0, 11), // "WaveManager"
QT_MOC_LITERAL(1, 12, 12), // "waveReceived"
QT_MOC_LITERAL(2, 25, 0), // ""
QT_MOC_LITERAL(3, 26, 9), // "orderCode"
QT_MOC_LITERAL(4, 36, 8), // "skuCount"
QT_MOC_LITERAL(5, 45, 18), // "waveSortingStarted"
QT_MOC_LITERAL(6, 64, 17), // "waveReadyToReport"
QT_MOC_LITERAL(7, 82, 17), // "waveStatusChanged"
QT_MOC_LITERAL(8, 100, 9), // "newStatus"
QT_MOC_LITERAL(9, 110, 10), // "codeMarked"
QT_MOC_LITERAL(10, 121, 4), // "code"
QT_MOC_LITERAL(11, 126, 6) // "sorted"

    },
    "WaveManager\0waveReceived\0\0orderCode\0"
    "skuCount\0waveSortingStarted\0"
    "waveReadyToReport\0waveStatusChanged\0"
    "newStatus\0codeMarked\0code\0sorted"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_WaveManager[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       5,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       5,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,   39,    2, 0x06 /* Public */,
       5,    1,   44,    2, 0x06 /* Public */,
       6,    1,   47,    2, 0x06 /* Public */,
       7,    1,   50,    2, 0x06 /* Public */,
       9,    2,   53,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString, QMetaType::Int,    3,    4,
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::Int,    8,
    QMetaType::Void, QMetaType::QString, QMetaType::Bool,   10,   11,

       0        // eod
};

void WaveManager::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<WaveManager *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->waveReceived((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 1: _t->waveSortingStarted((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->waveReadyToReport((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->waveStatusChanged((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 4: _t->codeMarked((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (WaveManager::*)(const QString & , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&WaveManager::waveReceived)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (WaveManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&WaveManager::waveSortingStarted)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (WaveManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&WaveManager::waveReadyToReport)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (WaveManager::*)(int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&WaveManager::waveStatusChanged)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (WaveManager::*)(const QString & , bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&WaveManager::codeMarked)) {
                *result = 4;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject WaveManager::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_WaveManager.data,
    qt_meta_data_WaveManager,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *WaveManager::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *WaveManager::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_WaveManager.stringdata0))
        return static_cast<void*>(this);
    return QObject::qt_metacast(_clname);
}

int WaveManager::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 5)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 5;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 5)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 5;
    }
    return _id;
}

// SIGNAL 0
void WaveManager::waveReceived(const QString & _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void WaveManager::waveSortingStarted(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void WaveManager::waveReadyToReport(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void WaveManager::waveStatusChanged(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void WaveManager::codeMarked(const QString & _t1, bool _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
