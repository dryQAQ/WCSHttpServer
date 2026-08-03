/****************************************************************************
** Meta object code from reading C++ file 'PlcManager.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../PlcManager.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#include <QtCore/QVector>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'PlcManager.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_PlcManager_t {
    QByteArrayData data[28];
    char stringdata0[320];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_PlcManager_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_PlcManager_t qt_meta_stringdata_PlcManager = {
    {
QT_MOC_LITERAL(0, 0, 10), // "PlcManager"
QT_MOC_LITERAL(1, 11, 12), // "plcConnected"
QT_MOC_LITERAL(2, 24, 0), // ""
QT_MOC_LITERAL(3, 25, 2), // "ip"
QT_MOC_LITERAL(4, 28, 4), // "port"
QT_MOC_LITERAL(5, 33, 15), // "plcDisconnected"
QT_MOC_LITERAL(6, 49, 19), // "plcFeedbackReceived"
QT_MOC_LITERAL(7, 69, 4), // "code"
QT_MOC_LITERAL(8, 74, 4), // "grid"
QT_MOC_LITERAL(9, 79, 3), // "car"
QT_MOC_LITERAL(10, 83, 16), // "plcFeedbackBatch"
QT_MOC_LITERAL(11, 100, 25), // "QVector<PlcFeedbackEntry>"
QT_MOC_LITERAL(12, 126, 7), // "entries"
QT_MOC_LITERAL(13, 134, 24), // "plcFeedbackBusinessBatch"
QT_MOC_LITERAL(14, 159, 13), // "plcBatchStart"
QT_MOC_LITERAL(15, 173, 12), // "plcBatchStop"
QT_MOC_LITERAL(16, 186, 11), // "plcSendInfo"
QT_MOC_LITERAL(17, 198, 5), // "grids"
QT_MOC_LITERAL(18, 204, 7), // "success"
QT_MOC_LITERAL(19, 212, 11), // "s7Connected"
QT_MOC_LITERAL(20, 224, 14), // "s7Disconnected"
QT_MOC_LITERAL(21, 239, 7), // "s7Error"
QT_MOC_LITERAL(22, 247, 6), // "errMsg"
QT_MOC_LITERAL(23, 254, 10), // "gridLocked"
QT_MOC_LITERAL(24, 265, 7), // "gridNum"
QT_MOC_LITERAL(25, 273, 12), // "gridUnlocked"
QT_MOC_LITERAL(26, 286, 15), // "gridLockedByPlc"
QT_MOC_LITERAL(27, 302, 17) // "gridUnlockedByPlc"

    },
    "PlcManager\0plcConnected\0\0ip\0port\0"
    "plcDisconnected\0plcFeedbackReceived\0"
    "code\0grid\0car\0plcFeedbackBatch\0"
    "QVector<PlcFeedbackEntry>\0entries\0"
    "plcFeedbackBusinessBatch\0plcBatchStart\0"
    "plcBatchStop\0plcSendInfo\0grids\0success\0"
    "s7Connected\0s7Disconnected\0s7Error\0"
    "errMsg\0gridLocked\0gridNum\0gridUnlocked\0"
    "gridLockedByPlc\0gridUnlockedByPlc"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_PlcManager[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      15,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      15,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    2,   89,    2, 0x06 /* Public */,
       5,    2,   94,    2, 0x06 /* Public */,
       6,    3,   99,    2, 0x06 /* Public */,
      10,    1,  106,    2, 0x06 /* Public */,
      13,    1,  109,    2, 0x06 /* Public */,
      14,    0,  112,    2, 0x06 /* Public */,
      15,    0,  113,    2, 0x06 /* Public */,
      16,    3,  114,    2, 0x06 /* Public */,
      19,    1,  121,    2, 0x06 /* Public */,
      20,    1,  124,    2, 0x06 /* Public */,
      21,    1,  127,    2, 0x06 /* Public */,
      23,    1,  130,    2, 0x06 /* Public */,
      25,    1,  133,    2, 0x06 /* Public */,
      26,    1,  136,    2, 0x06 /* Public */,
      27,    1,  139,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString, QMetaType::Int,    3,    4,
    QMetaType::Void, QMetaType::QString, QMetaType::Int,    3,    4,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QString,    7,    8,    9,
    QMetaType::Void, 0x80000000 | 11,   12,
    QMetaType::Void, 0x80000000 | 11,   12,
    QMetaType::Void,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::Bool,    7,   17,   18,
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::QString,    3,
    QMetaType::Void, QMetaType::QString,   22,
    QMetaType::Void, QMetaType::QString,   24,
    QMetaType::Void, QMetaType::QString,   24,
    QMetaType::Void, QMetaType::QString,   24,
    QMetaType::Void, QMetaType::QString,   24,

       0        // eod
};

void PlcManager::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<PlcManager *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->plcConnected((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 1: _t->plcDisconnected((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 2: _t->plcFeedbackReceived((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
        case 3: _t->plcFeedbackBatch((*reinterpret_cast< const QVector<PlcFeedbackEntry>(*)>(_a[1]))); break;
        case 4: _t->plcFeedbackBusinessBatch((*reinterpret_cast< const QVector<PlcFeedbackEntry>(*)>(_a[1]))); break;
        case 5: _t->plcBatchStart(); break;
        case 6: _t->plcBatchStop(); break;
        case 7: _t->plcSendInfo((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< bool(*)>(_a[3]))); break;
        case 8: _t->s7Connected((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 9: _t->s7Disconnected((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 10: _t->s7Error((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 11: _t->gridLocked((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 12: _t->gridUnlocked((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 13: _t->gridLockedByPlc((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 14: _t->gridUnlockedByPlc((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
        case 3:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<PlcFeedbackEntry> >(); break;
            }
            break;
        case 4:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 0:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QVector<PlcFeedbackEntry> >(); break;
            }
            break;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (PlcManager::*)(const QString & , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::plcConnected)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QString & , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::plcDisconnected)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QString & , const QString & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::plcFeedbackReceived)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QVector<PlcFeedbackEntry> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::plcFeedbackBatch)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QVector<PlcFeedbackEntry> & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::plcFeedbackBusinessBatch)) {
                *result = 4;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::plcBatchStart)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::plcBatchStop)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QString & , const QString & , bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::plcSendInfo)) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::s7Connected)) {
                *result = 8;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::s7Disconnected)) {
                *result = 9;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::s7Error)) {
                *result = 10;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::gridLocked)) {
                *result = 11;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::gridUnlocked)) {
                *result = 12;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::gridLockedByPlc)) {
                *result = 13;
                return;
            }
        }
        {
            using _t = void (PlcManager::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&PlcManager::gridUnlockedByPlc)) {
                *result = 14;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject PlcManager::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_PlcManager.data,
    qt_meta_data_PlcManager,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *PlcManager::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *PlcManager::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_PlcManager.stringdata0))
        return static_cast<void*>(this);
    if (!strcmp(_clname, "CTcpServerListener"))
        return static_cast< CTcpServerListener*>(this);
    return QObject::qt_metacast(_clname);
}

int PlcManager::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 15)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 15;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 15)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 15;
    }
    return _id;
}

// SIGNAL 0
void PlcManager::plcConnected(const QString & _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void PlcManager::plcDisconnected(const QString & _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void PlcManager::plcFeedbackReceived(const QString & _t1, const QString & _t2, const QString & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void PlcManager::plcFeedbackBatch(const QVector<PlcFeedbackEntry> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 4
void PlcManager::plcFeedbackBusinessBatch(const QVector<PlcFeedbackEntry> & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 4, _a);
}

// SIGNAL 5
void PlcManager::plcBatchStart()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void PlcManager::plcBatchStop()
{
    QMetaObject::activate(this, &staticMetaObject, 6, nullptr);
}

// SIGNAL 7
void PlcManager::plcSendInfo(const QString & _t1, const QString & _t2, bool _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}

// SIGNAL 8
void PlcManager::s7Connected(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 8, _a);
}

// SIGNAL 9
void PlcManager::s7Disconnected(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 9, _a);
}

// SIGNAL 10
void PlcManager::s7Error(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 10, _a);
}

// SIGNAL 11
void PlcManager::gridLocked(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 11, _a);
}

// SIGNAL 12
void PlcManager::gridUnlocked(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 12, _a);
}

// SIGNAL 13
void PlcManager::gridLockedByPlc(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 13, _a);
}

// SIGNAL 14
void PlcManager::gridUnlockedByPlc(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 14, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
