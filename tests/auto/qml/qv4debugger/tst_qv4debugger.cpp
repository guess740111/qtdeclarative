/****************************************************************************
**
** Copyright (C) 2015 The Qt Company Ltd.
** Contact: http://www.qt.io/licensing/
**
** This file is part of the test suite of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL21$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see http://www.qt.io/terms-conditions. For further
** information use the contact form at http://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 2.1 or version 3 as published by the Free
** Software Foundation and appearing in the file LICENSE.LGPLv21 and
** LICENSE.LGPLv3 included in the packaging of this file. Please review the
** following information to ensure the GNU Lesser General Public License
** requirements will be met: https://www.gnu.org/licenses/lgpl.html and
** http://www.gnu.org/licenses/old-licenses/lgpl-2.1.html.
**
** As a special exception, The Qt Company gives you certain additional
** rights. These rights are described in The Qt Company LGPL Exception
** version 1.1, included in the file LGPL_EXCEPTION.txt in this package.
**
** $QT_END_LICENSE$
**
****************************************************************************/
#include <QtTest/QtTest>

#include "qv4datacollector.h"

#include <QJSEngine>
#include <QQmlEngine>
#include <QQmlComponent>
#include <private/qv4engine_p.h>
#include <private/qv4debugging_p.h>
#include <private/qv8engine_p.h>
#include <private/qv4objectiterator_p.h>

using namespace QV4;
using namespace QV4::Debugging;

typedef QV4::ReturnedValue (*InjectedFunction)(QV4::CallContext*);
Q_DECLARE_METATYPE(InjectedFunction)

static bool waitForSignal(QObject* obj, const char* signal, int timeout = 10000)
{
    QEventLoop loop;
    QObject::connect(obj, signal, &loop, SLOT(quit()));
    QTimer timer;
    QSignalSpy timeoutSpy(&timer, SIGNAL(timeout()));
    if (timeout > 0) {
        QObject::connect(&timer, SIGNAL(timeout()), &loop, SLOT(quit()));
        timer.setSingleShot(true);
        timer.start(timeout);
    }
    loop.exec();
    return timeoutSpy.isEmpty();
}

class TestEngine : public QJSEngine
{
    Q_OBJECT
public:
    TestEngine()
    {
        qMetaTypeId<InjectedFunction>();
    }

    Q_INVOKABLE void evaluate(const QString &script, const QString &fileName, int lineNumber = 1)
    {
        QJSEngine::evaluate(script, fileName, lineNumber);
        emit evaluateFinished();
    }

    QV4::ExecutionEngine *v4Engine() { return QV8Engine::getV4(this); }

    Q_INVOKABLE void injectFunction(const QString &functionName, InjectedFunction injectedFunction)
    {
        QV4::ExecutionEngine *v4 = v4Engine();
        QV4::Scope scope(v4);

        QV4::ScopedString name(scope, v4->newString(functionName));
        QV4::ScopedContext ctx(scope, v4->rootContext());
        QV4::ScopedValue function(scope, BuiltinFunction::create(ctx, name, injectedFunction));
        v4->globalObject->put(name, function);
    }

signals:
    void evaluateFinished();
};

class TestAgent : public QObject
{
    Q_OBJECT
public:
    typedef QV4DataCollector::Refs Refs;
    typedef QV4DataCollector::Ref Ref;
    struct NamedRefs {
        NamedRefs(QV4DataCollector *collector = 0): collector(collector) {}

        QStringList names;
        Refs refs;
        QV4DataCollector *collector;

        int size() const {
            Q_ASSERT(names.size() == refs.size());
            return names.size();
        }

        bool contains(const QString &name) const {
            return names.contains(name);
        }

#define DUMP_JSON(x) {\
    QJsonDocument doc(x);\
    qDebug() << #x << "=" << doc.toJson(QJsonDocument::Indented);\
}

        QJsonObject rawValue(const QString &name) const {
            Q_ASSERT(contains(name));
            return collector->lookupRef(refs.at(names.indexOf(name)));
        }

        QJsonValue value(const QString &name) const {
            return rawValue(name).value(QStringLiteral("value"));
        }

        QString type(const QString &name) const {
            return rawValue(name).value(QStringLiteral("type")).toString();
        }

        void dump(const QString &name) const {
            if (!contains(name)) {
                qDebug() << "no" << name;
                return;
            }

            QJsonObject o = collector->lookupRef(refs.at(names.indexOf(name)));
            QJsonDocument d;
            d.setObject(o);
            qDebug() << name << "=" << d.toJson(QJsonDocument::Indented);
        }
    };

    TestAgent(QV4::ExecutionEngine *engine)
        : m_wasPaused(false)
        , m_captureContextInfo(false)
        , m_thrownValue(-1)
        , collector(engine)
        , m_debugger(0)
    {
    }

public slots:
    void debuggerPaused(QV4::Debugging::Debugger *debugger, QV4::Debugging::PauseReason reason)
    {
        Q_ASSERT(debugger == m_debugger);
        Q_ASSERT(debugger->engine() == collector.engine());
        m_wasPaused = true;
        m_pauseReason = reason;
        m_statesWhenPaused << debugger->currentExecutionState();

        if (debugger->state() == QV4::Debugging::Debugger::Paused &&
                debugger->engine()->hasException) {
            Refs refs;
            RefHolder holder(&collector, &refs);
            ExceptionCollectJob job(debugger->engine(), &collector);
            debugger->runInEngine(&job);
            Q_ASSERT(refs.size() > 0);
            m_thrownValue = refs.first();
        }

        foreach (const TestBreakPoint &bp, m_breakPointsToAddWhenPaused)
            debugger->addBreakPoint(bp.fileName, bp.lineNumber);
        m_breakPointsToAddWhenPaused.clear();

        m_stackTrace = debugger->stackTrace();

        while (!m_expressionRequests.isEmpty()) {
            Q_ASSERT(debugger->state() == QV4::Debugging::Debugger::Paused);
            ExpressionRequest request = m_expressionRequests.takeFirst();
            m_expressionResults << Refs();
            RefHolder holder(&collector, &m_expressionResults.last());
            ExpressionEvalJob job(debugger->engine(), request.frameNr, request.expression,
                                  &collector);
            debugger->runInEngine(&job);
        }

        if (m_captureContextInfo)
            captureContextInfo(debugger);

        debugger->resume(Debugger::FullThrottle);
    }

public:
    struct TestBreakPoint
    {
        TestBreakPoint() : lineNumber(-1) {}
        TestBreakPoint(const QString &fileName, int lineNumber)
            : fileName(fileName), lineNumber(lineNumber) {}
        QString fileName;
        int lineNumber;
    };

    void captureContextInfo(Debugger *debugger)
    {
        for (int i = 0, ei = m_stackTrace.size(); i != ei; ++i) {
            m_capturedArguments.append(NamedRefs(&collector));
            RefHolder argHolder(&collector, &m_capturedArguments.last().refs);
            ArgumentCollectJob argumentsJob(debugger->engine(), &collector,
                                            &m_capturedArguments.last().names, i, 0);
            debugger->runInEngine(&argumentsJob);

            m_capturedLocals.append(NamedRefs(&collector));
            RefHolder localHolder(&collector, &m_capturedLocals.last().refs);
            LocalCollectJob localsJob(debugger->engine(), &collector,
                                      &m_capturedLocals.last().names, i, 0);
            debugger->runInEngine(&localsJob);
        }
    }

    void addDebugger(QV4::Debugging::Debugger *debugger)
    {
        Q_ASSERT(!m_debugger);
        m_debugger = debugger;
        connect(m_debugger,
                SIGNAL(debuggerPaused(QV4::Debugging::Debugger*,QV4::Debugging::PauseReason)),
                this,
                SLOT(debuggerPaused(QV4::Debugging::Debugger*,QV4::Debugging::PauseReason)));
    }

    bool m_wasPaused;
    PauseReason m_pauseReason;
    bool m_captureContextInfo;
    QList<Debugger::ExecutionState> m_statesWhenPaused;
    QList<TestBreakPoint> m_breakPointsToAddWhenPaused;
    QVector<QV4::StackFrame> m_stackTrace;
    QVector<NamedRefs> m_capturedArguments;
    QVector<NamedRefs> m_capturedLocals;
    qint64 m_thrownValue;
    QV4DataCollector collector;

    struct ExpressionRequest {
        QString expression;
        int frameNr;
    };
    QVector<ExpressionRequest> m_expressionRequests;
    QVector<Refs> m_expressionResults;
    QV4::Debugging::Debugger *m_debugger;

    // Utility methods:
    void dumpStackTrace() const
    {
        qDebug() << "Stack depth:" << m_stackTrace.size();
        foreach (const QV4::StackFrame &frame, m_stackTrace)
            qDebug("\t%s (%s:%d:%d)", qPrintable(frame.function), qPrintable(frame.source),
                   frame.line, frame.column);
    }
};

class tst_qv4debugger : public QObject
{
    Q_OBJECT

private slots:
    void init();
    void cleanup();

    // breakpoints:
    void breakAnywhere();
    void pendingBreakpoint();
    void liveBreakPoint();
    void removePendingBreakPoint();
    void addBreakPointWhilePaused();
    void removeBreakPointForNextInstruction();
    void conditionalBreakPoint();
    void conditionalBreakPointInQml();

    // context access:
    void readArguments();
    void readLocals();
    void readObject();
    void readContextInAllFrames();

    // exceptions:
    void pauseOnThrow();
    void breakInCatch();
    void breakInWith();

    void evaluateExpression();

private:
    void evaluateJavaScript(const QString &script, const QString &fileName, int lineNumber = 1)
    {
        QMetaObject::invokeMethod(m_engine, "evaluate", Qt::QueuedConnection,
                                  Q_ARG(QString, script), Q_ARG(QString, fileName),
                                  Q_ARG(int, lineNumber));
        waitForSignal(m_engine, SIGNAL(evaluateFinished()), /*timeout*/0);
    }

    TestEngine *m_engine;
    QV4::ExecutionEngine *m_v4;
    TestAgent *m_debuggerAgent;
    QThread *m_javaScriptThread;
};

void tst_qv4debugger::init()
{
    m_javaScriptThread = new QThread;
    m_engine = new TestEngine;
    m_v4 = m_engine->v4Engine();
    m_v4->enableDebugger();
    m_engine->moveToThread(m_javaScriptThread);
    m_javaScriptThread->start();
    m_debuggerAgent = new TestAgent(m_v4);
    m_debuggerAgent->addDebugger(m_v4->debugger);
}

void tst_qv4debugger::cleanup()
{
    m_javaScriptThread->exit();
    m_javaScriptThread->wait();
    delete m_engine;
    delete m_javaScriptThread;
    m_engine = 0;
    m_v4 = 0;
    delete m_debuggerAgent;
    m_debuggerAgent = 0;
}

void tst_qv4debugger::breakAnywhere()
{
    QString script =
            "var i = 42;\n"
            "var j = i + 1\n"
            "var k = i\n";
    m_v4->debugger->pause();
    evaluateJavaScript(script, "testFile");
    QVERIFY(m_debuggerAgent->m_wasPaused);
}

void tst_qv4debugger::pendingBreakpoint()
{
    QString script =
            "var i = 42;\n"
            "var j = i + 1\n"
            "var k = i\n";
    m_v4->debugger->addBreakPoint("testfile", 2);
    evaluateJavaScript(script, "testfile");
    QVERIFY(m_debuggerAgent->m_wasPaused);
    QCOMPARE(m_debuggerAgent->m_statesWhenPaused.count(), 1);
    QV4::Debugging::Debugger::ExecutionState state = m_debuggerAgent->m_statesWhenPaused.first();
    QCOMPARE(state.fileName, QString("testfile"));
    QCOMPARE(state.lineNumber, 2);
}

void tst_qv4debugger::liveBreakPoint()
{
    QString script =
            "var i = 42;\n"
            "var j = i + 1\n"
            "var k = i\n";
    m_debuggerAgent->m_breakPointsToAddWhenPaused << TestAgent::TestBreakPoint("liveBreakPoint", 3);
    m_v4->debugger->pause();
    evaluateJavaScript(script, "liveBreakPoint");
    QVERIFY(m_debuggerAgent->m_wasPaused);
    QCOMPARE(m_debuggerAgent->m_statesWhenPaused.count(), 2);
    QV4::Debugging::Debugger::ExecutionState state = m_debuggerAgent->m_statesWhenPaused.at(1);
    QCOMPARE(state.fileName, QString("liveBreakPoint"));
    QCOMPARE(state.lineNumber, 3);
}

void tst_qv4debugger::removePendingBreakPoint()
{
    QString script =
            "var i = 42;\n"
            "var j = i + 1\n"
            "var k = i\n";
    m_v4->debugger->addBreakPoint("removePendingBreakPoint", 2);
    m_v4->debugger->removeBreakPoint("removePendingBreakPoint", 2);
    evaluateJavaScript(script, "removePendingBreakPoint");
    QVERIFY(!m_debuggerAgent->m_wasPaused);
}

void tst_qv4debugger::addBreakPointWhilePaused()
{
    QString script =
            "var i = 42;\n"
            "var j = i + 1\n"
            "var k = i\n";
    m_v4->debugger->addBreakPoint("addBreakPointWhilePaused", 1);
    m_debuggerAgent->m_breakPointsToAddWhenPaused << TestAgent::TestBreakPoint("addBreakPointWhilePaused", 2);
    evaluateJavaScript(script, "addBreakPointWhilePaused");
    QVERIFY(m_debuggerAgent->m_wasPaused);
    QCOMPARE(m_debuggerAgent->m_statesWhenPaused.count(), 2);

    QV4::Debugging::Debugger::ExecutionState state = m_debuggerAgent->m_statesWhenPaused.at(0);
    QCOMPARE(state.fileName, QString("addBreakPointWhilePaused"));
    QCOMPARE(state.lineNumber, 1);

    state = m_debuggerAgent->m_statesWhenPaused.at(1);
    QCOMPARE(state.fileName, QString("addBreakPointWhilePaused"));
    QCOMPARE(state.lineNumber, 2);
}

static QV4::ReturnedValue someCall(QV4::CallContext *ctx)
{
    ctx->d()->engine->debugger->removeBreakPoint("removeBreakPointForNextInstruction", 2);
    return QV4::Encode::undefined();
}

void tst_qv4debugger::removeBreakPointForNextInstruction()
{
    QString script =
            "someCall();\n"
            "var i = 42;";

    QMetaObject::invokeMethod(m_engine, "injectFunction", Qt::BlockingQueuedConnection,
                              Q_ARG(QString, "someCall"), Q_ARG(InjectedFunction, someCall));

    m_v4->debugger->addBreakPoint("removeBreakPointForNextInstruction", 2);

    evaluateJavaScript(script, "removeBreakPointForNextInstruction");
    QVERIFY(!m_debuggerAgent->m_wasPaused);
}

void tst_qv4debugger::conditionalBreakPoint()
{
    m_debuggerAgent->m_captureContextInfo = true;
    QString script =
            "function test() {\n"
            "    for (var i = 0; i < 15; ++i) {\n"
            "        var x = i;\n"
            "    }\n"
            "}\n"
            "test()\n";

    m_v4->debugger->addBreakPoint("conditionalBreakPoint", 3, QStringLiteral("i > 10"));
    evaluateJavaScript(script, "conditionalBreakPoint");
    QVERIFY(m_debuggerAgent->m_wasPaused);
    QCOMPARE(m_debuggerAgent->m_statesWhenPaused.count(), 4);
    QV4::Debugging::Debugger::ExecutionState state = m_debuggerAgent->m_statesWhenPaused.first();
    QCOMPARE(state.fileName, QString("conditionalBreakPoint"));
    QCOMPARE(state.lineNumber, 3);

    QVERIFY(m_debuggerAgent->m_capturedLocals.size() > 1);
    const TestAgent::NamedRefs &frame0 = m_debuggerAgent->m_capturedLocals.at(0);
    QCOMPARE(frame0.size(), 2);
    QVERIFY(frame0.contains("i"));
    QCOMPARE(frame0.value("i").toInt(), 11);
}

void tst_qv4debugger::conditionalBreakPointInQml()
{
    QQmlEngine engine;
    QV4::ExecutionEngine *v4 = QV8Engine::getV4(&engine);
    v4->enableDebugger();

    QScopedPointer<QThread> debugThread(new QThread);
    debugThread->start();
    QScopedPointer<TestAgent> debuggerAgent(new TestAgent(v4));
    debuggerAgent->addDebugger(v4->debugger);
    debuggerAgent->moveToThread(debugThread.data());

    QQmlComponent component(&engine);
    component.setData("import QtQml 2.0\n"
                      "QtObject {\n"
                      "    id: root\n"
                      "    property int foo: 42\n"
                      "    property bool success: false\n"
                      "    Component.onCompleted: {\n"
                      "        success = true;\n" // breakpoint here
                      "    }\n"
                      "}\n", QUrl("test.qml"));

    v4->debugger->addBreakPoint("test.qml", 7, "root.foo == 42");

    QScopedPointer<QObject> obj(component.create());
    QCOMPARE(obj->property("success").toBool(), true);

    QCOMPARE(debuggerAgent->m_statesWhenPaused.count(), 1);
    QCOMPARE(debuggerAgent->m_statesWhenPaused.at(0).fileName, QStringLiteral("test.qml"));
    QCOMPARE(debuggerAgent->m_statesWhenPaused.at(0).lineNumber, 7);

    debugThread->quit();
    debugThread->wait();
}

void tst_qv4debugger::readArguments()
{
    m_debuggerAgent->m_captureContextInfo = true;
    QString script =
            "function f(a, b, c, d) {\n"
            "  return a === b\n"
            "}\n"
            "var four;\n"
            "f(1, 'two', null, four);\n";
    m_v4->debugger->addBreakPoint("readArguments", 2);
    evaluateJavaScript(script, "readArguments");
    QVERIFY(m_debuggerAgent->m_wasPaused);
    QVERIFY(m_debuggerAgent->m_capturedArguments.size() > 1);
    const TestAgent::NamedRefs &frame0 = m_debuggerAgent->m_capturedArguments.at(0);
    QCOMPARE(frame0.size(), 4);
    QVERIFY(frame0.contains(QStringLiteral("a")));
    QCOMPARE(frame0.type(QStringLiteral("a")), QStringLiteral("number"));
    QCOMPARE(frame0.value(QStringLiteral("a")).toDouble(), 1.0);
    QVERIFY(frame0.names.contains("b"));
    QCOMPARE(frame0.type(QStringLiteral("b")), QStringLiteral("string"));
    QCOMPARE(frame0.value(QStringLiteral("b")).toString(), QStringLiteral("two"));
}

void tst_qv4debugger::readLocals()
{
    m_debuggerAgent->m_captureContextInfo = true;
    QString script =
            "function f(a, b) {\n"
            "  var c = a + b\n"
            "  var d = a - b\n" // breakpoint, c should be set, d should be undefined
            "  return c === d\n"
            "}\n"
            "f(1, 2, 3);\n";
    m_v4->debugger->addBreakPoint("readLocals", 3);
    evaluateJavaScript(script, "readLocals");
    QVERIFY(m_debuggerAgent->m_wasPaused);
    QVERIFY(m_debuggerAgent->m_capturedLocals.size() > 1);
    const TestAgent::NamedRefs &frame0 = m_debuggerAgent->m_capturedLocals.at(0);
    QCOMPARE(frame0.size(), 2);
    QVERIFY(frame0.contains("c"));
    QCOMPARE(frame0.type("c"), QStringLiteral("number"));
    QCOMPARE(frame0.value("c").toDouble(), 3.0);
    QVERIFY(frame0.contains("d"));
    QCOMPARE(frame0.type("d"), QStringLiteral("undefined"));
}

void tst_qv4debugger::readObject()
{
    m_debuggerAgent->m_captureContextInfo = true;
    QString script =
            "function f(a) {\n"
            "  var b = a\n"
            "  return b\n"
            "}\n"
            "f({head: 1, tail: { head: 'asdf', tail: null }});\n";
    m_v4->debugger->addBreakPoint("readObject", 3);
    evaluateJavaScript(script, "readObject");
    QVERIFY(m_debuggerAgent->m_wasPaused);
    QVERIFY(m_debuggerAgent->m_capturedLocals.size() > 1);
    const TestAgent::NamedRefs &frame0 = m_debuggerAgent->m_capturedLocals.at(0);
    QCOMPARE(frame0.size(), 1);
    QVERIFY(frame0.contains("b"));
    QCOMPARE(frame0.type("b"), QStringLiteral("object"));
    QJsonObject b = frame0.rawValue("b");
    QVERIFY(b.contains(QStringLiteral("properties")));
    QVERIFY(b.value("properties").isArray());
    QJsonArray b_props = b.value("properties").toArray();
    QCOMPARE(b_props.size(), 2);

    QVERIFY(b_props.at(0).isObject());
    QJsonObject b_head = b_props.at(0).toObject();
    QCOMPARE(b_head.value("name").toString(), QStringLiteral("head"));
    QCOMPARE(b_head.value("type").toString(), QStringLiteral("number"));
    QCOMPARE(b_head.value("value").toDouble(), 1.0);
    QVERIFY(b_props.at(1).isObject());
    QJsonObject b_tail = b_props.at(1).toObject();
    QCOMPARE(b_tail.value("name").toString(), QStringLiteral("tail"));
    QVERIFY(b_tail.contains("ref"));

    QJsonObject b_tail_value = frame0.collector->lookupRef(b_tail.value("ref").toInt());
    QCOMPARE(b_tail_value.value("type").toString(), QStringLiteral("object"));
    QVERIFY(b_tail_value.contains("properties"));
    QJsonArray b_tail_props = b_tail_value.value("properties").toArray();
    QCOMPARE(b_tail_props.size(), 2);
    QJsonObject b_tail_head = b_tail_props.at(0).toObject();
    QCOMPARE(b_tail_head.value("name").toString(), QStringLiteral("head"));
    QCOMPARE(b_tail_head.value("type").toString(), QStringLiteral("string"));
    QCOMPARE(b_tail_head.value("value").toString(), QStringLiteral("asdf"));
    QJsonObject b_tail_tail = b_tail_props.at(1).toObject();
    QCOMPARE(b_tail_tail.value("name").toString(), QStringLiteral("tail"));
    QCOMPARE(b_tail_tail.value("type").toString(), QStringLiteral("null"));
    QVERIFY(b_tail_tail.value("value").isNull());
}

void tst_qv4debugger::readContextInAllFrames()
{
    m_debuggerAgent->m_captureContextInfo = true;
    QString script =
            "function fact(n) {\n"
            "  if (n > 1) {\n"
            "    var n_1 = n - 1;\n"
            "    n_1 = fact(n_1);\n"
            "    return n * n_1;\n"
            "  } else\n"
            "    return 1;\n" // breakpoint
            "}\n"
            "fact(12);\n";
    m_v4->debugger->addBreakPoint("readFormalsInAllFrames", 7);
    evaluateJavaScript(script, "readFormalsInAllFrames");
    QVERIFY(m_debuggerAgent->m_wasPaused);
    QCOMPARE(m_debuggerAgent->m_stackTrace.size(), 13);
    QCOMPARE(m_debuggerAgent->m_capturedArguments.size(), 13);
    QCOMPARE(m_debuggerAgent->m_capturedLocals.size(), 13);

    for (int i = 0; i < 12; ++i) {
        const TestAgent::NamedRefs &args = m_debuggerAgent->m_capturedArguments.at(i);
        QCOMPARE(args.size(), 1);
        QVERIFY(args.contains("n"));
        QCOMPARE(args.type("n"), QStringLiteral("number"));
        QCOMPARE(args.value("n").toDouble(), i + 1.0);

        const TestAgent::NamedRefs &locals = m_debuggerAgent->m_capturedLocals.at(i);
        QCOMPARE(locals.size(), 1);
        QVERIFY(locals.contains("n_1"));
        if (i == 0) {
            QCOMPARE(locals.type("n_1"), QStringLiteral("undefined"));
        } else {
            QCOMPARE(locals.type("n_1"), QStringLiteral("number"));
            QCOMPARE(locals.value("n_1").toInt(), i);
        }
    }
    QCOMPARE(m_debuggerAgent->m_capturedArguments[12].size(), 0);
    QCOMPARE(m_debuggerAgent->m_capturedLocals[12].size(), 0);
}

void tst_qv4debugger::pauseOnThrow()
{
    QString script =
            "function die(n) {\n"
            "  throw n\n"
            "}\n"
            "die('hard');\n";
    m_v4->debugger->setBreakOnThrow(true);
    evaluateJavaScript(script, "pauseOnThrow");
    QVERIFY(m_debuggerAgent->m_wasPaused);
    QCOMPARE(m_debuggerAgent->m_pauseReason, Throwing);
    QCOMPARE(m_debuggerAgent->m_stackTrace.size(), 2);
    QVERIFY(m_debuggerAgent->m_thrownValue >= qint64(0));
    QJsonObject exception = m_debuggerAgent->collector.lookupRef(m_debuggerAgent->m_thrownValue);
//    DUMP_JSON(exception);
    QCOMPARE(exception.value("type").toString(), QStringLiteral("string"));
    QCOMPARE(exception.value("value").toString(), QStringLiteral("hard"));
}

void tst_qv4debugger::breakInCatch()
{
    QString script =
            "try {\n"
            "    throw 'catch...'\n"
            "} catch (e) {\n"
            "    console.log(e, 'me');\n"
            "}\n";

    m_v4->debugger->addBreakPoint("breakInCatch", 4);
    evaluateJavaScript(script, "breakInCatch");
    QVERIFY(m_debuggerAgent->m_wasPaused);
    QCOMPARE(m_debuggerAgent->m_pauseReason, BreakPoint);
    QCOMPARE(m_debuggerAgent->m_statesWhenPaused.count(), 1);
    QV4::Debugging::Debugger::ExecutionState state = m_debuggerAgent->m_statesWhenPaused.first();
    QCOMPARE(state.fileName, QString("breakInCatch"));
    QCOMPARE(state.lineNumber, 4);
}

void tst_qv4debugger::breakInWith()
{
    QString script =
            "with (42) {\n"
            "    console.log('give the answer');\n"
            "}\n";

    m_v4->debugger->addBreakPoint("breakInWith", 2);
    evaluateJavaScript(script, "breakInWith");
    QVERIFY(m_debuggerAgent->m_wasPaused);
    QCOMPARE(m_debuggerAgent->m_pauseReason, BreakPoint);
    QCOMPARE(m_debuggerAgent->m_statesWhenPaused.count(), 1);
    QV4::Debugging::Debugger::ExecutionState state = m_debuggerAgent->m_statesWhenPaused.first();
    QCOMPARE(state.fileName, QString("breakInWith"));
    QCOMPARE(state.lineNumber, 2);
}

void tst_qv4debugger::evaluateExpression()
{
    QString script =
            "function testFunction() {\n"
            "    var x = 10\n"
            "    return x\n" // breakpoint
            "}\n"
            "var x = 20\n"
            "testFunction()\n";

    TestAgent::ExpressionRequest request;
    request.expression = "x";
    request.frameNr = 0;
    m_debuggerAgent->m_expressionRequests << request;
    request.expression = "x";
    request.frameNr = 1;
    m_debuggerAgent->m_expressionRequests << request;

    m_v4->debugger->addBreakPoint("evaluateExpression", 3);

    evaluateJavaScript(script, "evaluateExpression");

    QCOMPARE(m_debuggerAgent->m_expressionResults.count(), 2);
    QCOMPARE(m_debuggerAgent->m_expressionResults[0].size(), 1);
    QJsonObject result0 =
            m_debuggerAgent->collector.lookupRef(m_debuggerAgent->m_expressionResults[0].first());
    QCOMPARE(result0.value("type").toString(), QStringLiteral("number"));
    QCOMPARE(result0.value("value").toInt(), 10);
    QCOMPARE(m_debuggerAgent->m_expressionResults[1].size(), 1);
    QJsonObject result1 =
            m_debuggerAgent->collector.lookupRef(m_debuggerAgent->m_expressionResults[1].first());
    QCOMPARE(result1.value("type").toString(), QStringLiteral("number"));
    QCOMPARE(result1.value("value").toInt(), 20);
}

QTEST_MAIN(tst_qv4debugger)

#include "tst_qv4debugger.moc"
