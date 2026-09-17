/****************************************************************************
** Meta object code from reading C++ file 'ParseWorker.h'
**
** Created by: The Qt Meta Object Compiler version 67 (Qt 5.15.2)
**
** WARNING! All changes made in this file will be lost!
*****************************************************************************/

#include <memory>
#include "../../../ParseWorker.h"
#include <QtCore/qbytearray.h>
#include <QtCore/qmetatype.h>
#include <QtCore/QVector>
#include <QtCore/QSet>
#if !defined(Q_MOC_OUTPUT_REVISION)
#error "The header file 'ParseWorker.h' doesn't include <QObject>."
#elif Q_MOC_OUTPUT_REVISION != 67
#error "This file was generated using the moc from 5.15.2. It"
#error "cannot be used with the include files from this version of Qt."
#error "(The moc has changed too much.)"
#endif

QT_BEGIN_MOC_NAMESPACE
QT_WARNING_PUSH
QT_WARNING_DISABLE_DEPRECATED
struct qt_meta_stringdata_ParseWorker_t {
    QByteArrayData data[19];
    char stringdata0[212];
};
#define QT_MOC_LITERAL(idx, ofs, len) \
    Q_STATIC_BYTE_ARRAY_DATA_HEADER_INITIALIZER_WITH_OFFSET(len, \
    qptrdiff(offsetof(qt_meta_stringdata_ParseWorker_t, stringdata0) + ofs \
        - idx * sizeof(QByteArrayData)) \
    )
static const qt_meta_stringdata_ParseWorker_t qt_meta_stringdata_ParseWorker = {
    {
QT_MOC_LITERAL(0, 0, 11), // "ParseWorker"
QT_MOC_LITERAL(1, 12, 10), // "waveParsed"
QT_MOC_LITERAL(2, 23, 0), // ""
QT_MOC_LITERAL(3, 24, 9), // "orderCode"
QT_MOC_LITERAL(4, 34, 8), // "skuCount"
QT_MOC_LITERAL(5, 43, 8), // "orderQty"
QT_MOC_LITERAL(6, 52, 9), // "elapsedMs"
QT_MOC_LITERAL(7, 62, 13), // "QSet<QString>"
QT_MOC_LITERAL(8, 76, 7), // "recvSet"
QT_MOC_LITERAL(9, 84, 7), // "rawBody"
QT_MOC_LITERAL(10, 92, 48), // "QVector<QPair<QString,QVector..."
QT_MOC_LITERAL(11, 141, 8), // "skuPlans"
QT_MOC_LITERAL(12, 150, 7), // "planSum"
QT_MOC_LITERAL(13, 158, 10), // "parseError"
QT_MOC_LITERAL(14, 169, 8), // "errorMsg"
QT_MOC_LITERAL(15, 178, 14), // "parseException"
QT_MOC_LITERAL(16, 193, 3), // "epc"
QT_MOC_LITERAL(17, 197, 7), // "gridNum"
QT_MOC_LITERAL(18, 205, 6) // "reason"

    },
    "ParseWorker\0waveParsed\0\0orderCode\0"
    "skuCount\0orderQty\0elapsedMs\0QSet<QString>\0"
    "recvSet\0rawBody\0"
    "QVector<QPair<QString,QVector<PlanGridInput> > >\0"
    "skuPlans\0planSum\0parseError\0errorMsg\0"
    "parseException\0epc\0gridNum\0reason"
};
#undef QT_MOC_LITERAL

static const uint qt_meta_data_ParseWorker[] = {

 // content:
       8,       // revision
       0,       // classname
       0,    0, // classinfo
       3,   14, // methods
       0,    0, // properties
       0,    0, // enums/sets
       0,    0, // constructors
       0,       // flags
       3,       // signalCount

 // signals: name, argc, parameters, tag, flags
       1,    8,   29,    2, 0x06 /* Public */,
      13,    1,   46,    2, 0x06 /* Public */,
      15,    4,   49,    2, 0x06 /* Public */,

 // signals: parameters
    QMetaType::Void, QMetaType::QString, QMetaType::Int, QMetaType::Int, QMetaType::LongLong, 0x80000000 | 7, QMetaType::QByteArray, 0x80000000 | 10, QMetaType::Int,    3,    4,    5,    6,    8,    9,   11,   12,
    QMetaType::Void, QMetaType::QString,   14,
    QMetaType::Void, QMetaType::QString, QMetaType::QString, QMetaType::QString, QMetaType::QString,    3,   16,   17,   18,

       0        // eod
};

void ParseWorker::qt_static_metacall(QObject *_o, QMetaObject::Call _c, int _id, void **_a)
{
    if (_c == QMetaObject::InvokeMetaMethod) {
        auto *_t = static_cast<ParseWorker *>(_o);
        Q_UNUSED(_t)
        switch (_id) {
        case 0: _t->waveParsed((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< int(*)>(_a[2])),(*reinterpret_cast< int(*)>(_a[3])),(*reinterpret_cast< qint64(*)>(_a[4])),(*reinterpret_cast< const QSet<QString>(*)>(_a[5])),(*reinterpret_cast< const QByteArray(*)>(_a[6])),(*reinterpret_cast< const QVector<QPair<QString,QVector<PlanGridInput> > >(*)>(_a[7])),(*reinterpret_cast< int(*)>(_a[8]))); break;
        case 1: _t->parseError((*reinterpret_cast< const QString(*)>(_a[1]))); break;
        case 2: _t->parseException((*reinterpret_cast< const QString(*)>(_a[1])),(*reinterpret_cast< const QString(*)>(_a[2])),(*reinterpret_cast< const QString(*)>(_a[3])),(*reinterpret_cast< const QString(*)>(_a[4]))); break;
        default: ;
        }
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        switch (_id) {
        default: *reinterpret_cast<int*>(_a[0]) = -1; break;
        case 0:
            switch (*reinterpret_cast<int*>(_a[1])) {
            default: *reinterpret_cast<int*>(_a[0]) = -1; break;
            case 4:
                *reinterpret_cast<int*>(_a[0]) = qRegisterMetaType< QSet<QString> >(); break;
            }
            break;
        }
    } else if (_c == QMetaObject::IndexOfMethod) {
        int *result = reinterpret_cast<int *>(_a[0]);
        {
            using _t = void (ParseWorker::*)(const QString & , int , int , qint64 , const QSet<QString> & , const QByteArray & , const QVector<QPair<QString,QVector<PlanGridInput>> > & , int );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ParseWorker::waveParsed)) {
                *result = 0;
                return;
            }
        }
        {
            using _t = void (ParseWorker::*)(const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ParseWorker::parseError)) {
                *result = 1;
                return;
            }
        }
        {
            using _t = void (ParseWorker::*)(const QString & , const QString & , const QString & , const QString & );
            if (*reinterpret_cast<_t *>(_a[1]) == static_cast<_t>(&ParseWorker::parseException)) {
                *result = 2;
                return;
            }
        }
    }
}

QT_INIT_METAOBJECT const QMetaObject ParseWorker::staticMetaObject = { {
    QMetaObject::SuperData::link<QThread::staticMetaObject>(),
    qt_meta_stringdata_ParseWorker.data,
    qt_meta_data_ParseWorker,
    qt_static_metacall,
    nullptr,
    nullptr
} };


const QMetaObject *ParseWorker::metaObject() const
{
    return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
}

void *ParseWorker::qt_metacast(const char *_clname)
{
    if (!_clname) return nullptr;
    if (!strcmp(_clname, qt_meta_stringdata_ParseWorker.stringdata0))
        return static_cast<void*>(this);
    return QThread::qt_metacast(_clname);
}

int ParseWorker::qt_metacall(QMetaObject::Call _c, int _id, void **_a)
{
    _id = QThread::qt_metacall(_c, _id, _a);
    if (_id < 0)
        return _id;
    if (_c == QMetaObject::InvokeMetaMethod) {
        if (_id < 3)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 3;
    } else if (_c == QMetaObject::RegisterMethodArgumentMetaType) {
        if (_id < 3)
            qt_static_metacall(this, _c, _id, _a);
        _id -= 3;
    }
    return _id;
}

// SIGNAL 0
void ParseWorker::waveParsed(const QString & _t1, int _t2, int _t3, qint64 _t4, const QSet<QString> & _t5, const QByteArray & _t6, const QVector<QPair<QString,QVector<PlanGridInput>> > & _t7, int _t8)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t5))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t6))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t7))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t8))) };
    QMetaObject::activate(this, &staticMetaObject, 0, _a);
}

// SIGNAL 1
void ParseWorker::parseError(const QString & _t1)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))) };
    QMetaObject::activate(this, &staticMetaObject, 1, _a);
}

// SIGNAL 2
void ParseWorker::parseException(const QString & _t1, const QString & _t2, const QString & _t3, const QString & _t4)
{
    void *_a[] = { nullptr, const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t1))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t2))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t3))), const_cast<void*>(reinterpret_cast<const void*>(std::addressof(_t4))) };
    QMetaObject::activate(this, &staticMetaObject, 2, _a);
}
QT_WARNING_POP
QT_END_MOC_NAMESPACE
