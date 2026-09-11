/****************************************************************************
** Meta object code from reading C++ file 'HttpServer.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../HttpServer.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'HttpServer.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_HttpServer_t {
    QByteArrayData data[30];
    char stringdata0[377];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_HttpServer_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_HttpServer_t qt_meta_stringdata_HttpServer = {
    {
QT_MOC_LITERAL(0, 0, 10), // "HttpServer"
QT_MOC_LITERAL(1, 11, 13), // "serverStarted"
QT_MOC_LITERAL(2, 25, 0), // ""
QT_MOC_LITERAL(3, 26, 4), // "port"
QT_MOC_LITERAL(4, 31, 13), // "serverStopped"
QT_MOC_LITERAL(5, 45, 17), // "waveReadyToReport"
QT_MOC_LITERAL(6, 63, 9), // "orderCode"
QT_MOC_LITERAL(7, 73, 10), // "logMessage"
QT_MOC_LITERAL(8, 84, 3), // "msg"
QT_MOC_LITERAL(9, 88, 7), // "isError"
QT_MOC_LITERAL(10, 96, 14), // "bindingUpdated"
QT_MOC_LITERAL(11, 111, 19), // "gridLockReportReady"
QT_MOC_LITERAL(12, 131, 10), // "reportJson"
QT_MOC_LITERAL(13, 142, 23), // "waveCompleteReportReady"
QT_MOC_LITERAL(14, 166, 18), // "fullboxReportReady"
QT_MOC_LITERAL(15, 185, 7), // "payload"
QT_MOC_LITERAL(16, 193, 5), // "msgId"
QT_MOC_LITERAL(17, 199, 14), // "endReportReady"
QT_MOC_LITERAL(18, 214, 17), // "endReportFinished"
QT_MOC_LITERAL(19, 232, 23), // "wavePersistenceFinished"
QT_MOC_LITERAL(20, 256, 2), // "ok"
QT_MOC_LITERAL(21, 259, 8), // "skuCount"
QT_MOC_LITERAL(22, 268, 17), // "outboxResendReady"
QT_MOC_LITERAL(23, 286, 4), // "kind"
QT_MOC_LITERAL(24, 291, 18), // "outboxResendResult"
QT_MOC_LITERAL(25, 310, 7), // "success"
QT_MOC_LITERAL(26, 318, 11), // "waveResumed"
QT_MOC_LITERAL(27, 330, 6), // "status"
QT_MOC_LITERAL(28, 337, 19), // "pendingWavesChanged"
QT_MOC_LITERAL(29, 357, 19) // "outboxFailedChanged"

    },
    "HttpServer\0serverStarted\0\0port\0"
    "serverStopped\0waveReadyToReport\0"
    "orderCode\0logMessage\0msg\0isError\0"
    "bindingUpdated\0gridLockReportReady\0"
    "reportJson\0waveCompleteReportReady\0"
    "fullboxReportReady\0payload\0msgId\0"
    "endReportReady\0endReportFinished\0"
    "wavePersistenceFinished\0ok\0skuCount\0"
    "outboxResendReady\0kind\0outboxResendResult\0"
    "success\0waveResumed\0status\0"
    "pendingWavesChanged\0outboxFailedChanged"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_HttpServer[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
      17,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
      17,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    1,   99,    2, 0x06 /* Public */,
       4,    0,  102,    2, 0x06 /* Public */,
       5,    1,  103,    2, 0x06 /* Public */,
       7,    2,  106,    2, 0x06 /* Public */,
       7,    1,  111,    2, 0x26 /* Public | MethodCloned */,
      10,    0,  114,    2, 0x06 /* Public */,
      11,    1,  115,    2, 0x06 /* Public */,
      13,    1,  118,    2, 0x06 /* Public */,
      14,    2,  121,    2, 0x06 /* Public */,
      17,    2,  126,    2, 0x06 /* Public */,
      18,    0,  131,    2, 0x06 /* Public */,
      19,    3,  132,    2, 0x06 /* Public */,
      22,    3,  139,    2, 0x06 /* Public */,
      24,    4,  146,    2, 0x06 /* Public */,
      26,    2,  155,    2, 0x06 /* Public */,
      28,    0,  160,    2, 0x06 /* Public */,
      29,    0,  161,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::Int,    3,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString,    6,
    QMetaType::Void, QMetaType::QString, QMetaType::Bool,    8,    9,
    QMetaType::Void, QMetaType::QString,    8,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QJsonObject,   12,
    QMetaType::Void, QMetaType::QJsonObject,   12,
    QMetaType::Void, QMetaType::QJsonObject, QMetaType::QString,   15,   16,
    QMetaType::Void, QMetaType::QJsonObject, QMetaType::QString,   15,   16,
    QMetaType::Void,
    QMetaType::Void, QMetaType::QString, QMetaType::Bool, QMetaType::Int,    6,   20,   21,
    QMetaType::Void, QMetaType::QString, QMetaType::QJsonObject, QMetaType::QString,   23,   15,   16,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QString, QMetaType::Bool,    6,   23,   16,   25,
    QMetaType::Void, QMetaType::QString, QMetaType::Int,    6,   27,
    QMetaType::Void,
    QMetaType::Void,

       0        // eod
};

void HttpServer::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<HttpServer *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->serverStarted((*reinterpret_cast< int(*)>(_a[1]))); break;
        case 1: _t->serverStopped(); break;
        case 2: _t->waveReadyToReport((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 3: _t->logMessage((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2]))); break;
        case 4: _t->logMessage((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 5: _t->bindingUpdated(); break;
        case 6: _t->gridLockReportReady((*reinterpret_cast< const QJsonObject(*)>(_a[1]))); break;
        case 7: _t->waveCompleteReportReady((*reinterpret_cast< const QJsonObject(*)>(_a[1]))); break;
        case 8: _t->fullboxReportReady((*reinterpret_cast< const QJsonObject(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 9: _t->endReportReady((*reinterpret_cast< const QJsonObject(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2]))); break;
        case 10: _t->endReportFinished(); break;
        case 11: _t->wavePersistenceFinished((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< bool(*)>(_a[2])),(*reinterpret_cast< int(*)>(_a[3]))); break;
        case 12: _t->outboxResendReady((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QJsonObject(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3]))); break;
        case 13: _t->outboxResendResult((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3])),(*reinterpret_cast< bool(*)>(_a[4]))); break;
        case 14: _t->waveResumed((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2]))); break;
        case 15: _t->pendingWavesChanged(); break;
        case 16: _t->outboxFailedChanged(); break;
        default: ;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (HttpServer::*)(int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::serverStarted)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::serverStopped)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::waveReadyToReport)) {
                *result = 2;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)(const QString & , bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::logMessage)) {
                *result = 3;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::bindingUpdated)) {
                *result = 5;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)(const QJsonObject & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::gridLockReportReady)) {
                *result = 6;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)(const QJsonObject & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::waveCompleteReportReady)) {
                *result = 7;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)(const QJsonObject & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::fullboxReportReady)) {
                *result = 8;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)(const QJsonObject & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::endReportReady)) {
                *result = 9;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::endReportFinished)) {
                *result = 10;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)(const QString & , bool , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::wavePersistenceFinished)) {
                *result = 11;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)(const QString & , const QJsonObject & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::outboxResendReady)) {
                *result = 12;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)(const QString & , const QString & , const QString & , bool );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::outboxResendResult)) {
                *result = 13;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)(const QString & , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::waveResumed)) {
                *result = 14;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::pendingWavesChanged)) {
                *result = 15;
                return;
            }
        }
        {
            using _t = void (HttpServer::*)();
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&HttpServer::outboxFailedChanged)) {
                *result = 16;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject HttpServer::staticMetaObject = { {
    QMetaObject::SuperData::link<QObject::staticMetaObject>(),
    qt_meta_stringdata_HttpServer.data,
    qt_meta_data_HttpServer,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *HttpServer::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *HttpServer::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_HttpServer.stringdata0))
        return static_cast<void*>(this);
    if (!strcmp(_clname, "CHttpServerListener"))
        return static_cast< CHttpServerListener*>(this);
    return QObject::qt_metacast(_clname);
}

int HttpServer::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QObject::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 17)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 17;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 17)
            *reinterpret_cast<int*>(_a[0]) = -1;
        _id -= 17;
    }
    return _id;
}

// SIGNAL 0
void HttpServer::serverStarted(int _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void HttpServer::serverStopped()
{
    QMetaObject::activate(this, &staticMetaObject, 1, nullptr);
}

// SIGNAL 2
void HttpServer::waveReadyToReport(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}

// SIGNAL 3
void HttpServer::logMessage(const QString & _t1, bool _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 3, _a);
}

// SIGNAL 5
void HttpServer::bindingUpdated()
{
    QMetaObject::activate(this, &staticMetaObject, 5, nullptr);
}

// SIGNAL 6
void HttpServer::gridLockReportReady(const QJsonObject & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 6, _a);
}

// SIGNAL 7
void HttpServer::waveCompleteReportReady(const QJsonObject & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 7, _a);
}

// SIGNAL 8
void HttpServer::fullboxReportReady(const QJsonObject & _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 8, _a);
}

// SIGNAL 9
void HttpServer::endReportReady(const QJsonObject & _t1, const QString & _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 9, _a);
}

// SIGNAL 10
void HttpServer::endReportFinished()
{
    QMetaObject::activate(this, &staticMetaObject, 10, nullptr);
}

// SIGNAL 11
void HttpServer::wavePersistenceFinished(const QString & _t1, bool _t2, int _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 11, _a);
}

// SIGNAL 12
void HttpServer::outboxResendReady(const QString & _t1, const QJsonObject & _t2, const QString & _t3)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))) };
    QMetaObject::activate(this, &staticMetaObject, 12, _a);
}

// SIGNAL 13
void HttpServer::outboxResendResult(const QString & _t1, const QString & _t2, const QString & _t3, bool _t4)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))) };
    QMetaObject::activate(this, &staticMetaObject, 13, _a);
}

// SIGNAL 14
void HttpServer::waveResumed(const QString & _t1, int _t2)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))) };
    QMetaObject::activate(this, &staticMetaObject, 14, _a);
}

// SIGNAL 15
void HttpServer::pendingWavesChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 15, nullptr);
}

// SIGNAL 16
void HttpServer::outboxFailedChanged()
{
    QMetaObject::activate(this, &staticMetaObject, 16, nullptr);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
