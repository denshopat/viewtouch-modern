#include <catch2/catch_test_macros.hpp>

#include "app/pos_json.hh"
#include "layout_fixture.hh"
#include "layoutcontroller.hh"
#include "pos_fixture.hh"
#include "print/spooler.hh"
#include "print/ticket_printer.hh"
#include "print/tickets.hh"
#include "qt_catch.hh"

#include <QFile>
#include <QQmlApplicationEngine>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTest>

using namespace Qt::StringLiterals;
using namespace vt;

// A tuckshop: a food truck's counter where one order is several children's
// bags. A parent orders for two children, each a bag for Little Lunch and
// one for Big Lunch, and pays once. The order is rung in, then packed: an
// item goes in a bag, and a bag is a child's for a break; every bag is a
// slip of its own.

namespace {

app::PosData tuckshopData()
{
    app::PosData d = test::seedPosData();
    d.settings.serviceMode = "tuckshop";
    d.settings.truckNoLogin = true;
    // One printer packs the bags (a drink isn't the bar's here).
    for (core::MenuItem &m : d.menu)
        m.printer = "kitchen";
    return d;
}

QString readFile(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()) : QString();
}

// The lines of the open check: {name, seat, course}.
QStringList linesOf(const app::PosService &pos)
{
    QStringList out;
    for (const QVariant &v : pos.lines()) {
        const QVariantMap l = v.toMap();
        out << u"%1/%2/%3"_s.arg(l[u"name"_s].toString()).arg(l[u"seat"_s].toInt()).arg(l[u"course"_s].toInt());
    }
    return out;
}

// A visible item by its objectName (Repeater items are not QObject children).
QQuickItem *named(QQuickItem *root, const QString &name)
{
    for (QQuickItem *item : root->childItems()) {
        if (!item->isVisible())
            continue;
        if (item->objectName() == name)
            return item;
        if (QQuickItem *hit = named(item, name))
            return hit;
    }
    return nullptr;
}

} // namespace

TEST_CASE("Tuckshop: a food truck's counter, its breaks by name", "[tuckshop]")
{
    app::PosShared shared(tuckshopData(), nullptr);
    app::PosService pos(&shared, u"Tuckshop"_s);

    CHECK(pos.serviceMode() == u"tuckshop"_s);
    CHECK(pos.tuckshop());
    CHECK(pos.counterMode());
    CHECK(pos.loggedIn());
    CHECK(pos.userName() == u"Counter"_s);
    // Its screens are a truck's; `tuckshop` and the breaks say the rest.
    const QVariantMap info = pos.serviceInfo();
    CHECK(info[u"mode"_s].toString() == u"foodTruck"_s);
    CHECK(info[u"tuckshop"_s].toBool());
    CHECK(info[u"breaks"_s].toStringList() == QStringList{u"Little Lunch"_s, u"Big Lunch"_s});
    CHECK(info[u"numbers"_s].toBool());

    // A food truck isn't one.
    app::PosData truck = test::seedPosData();
    truck.settings.serviceMode = "foodTruck";
    app::PosShared truckShared(truck, nullptr);
    app::PosService other(&truckShared, u"Truck"_s);
    CHECK_FALSE(other.tuckshop());
    CHECK_FALSE(other.serviceInfo()[u"tuckshop"_s].toBool());
    CHECK(other.serviceInfo()[u"breaks"_s].toStringList().isEmpty());
}

// The whole order rung in, then packed: Sam's and Mia's, a bag each for
// Little Lunch and Big Lunch. Returns the lines' ids, as rung in.
static QList<qint64> ringAndPack(app::PosService &pos)
{
    for (const QString &item : {u"classic-burger"_s, u"water"_s, u"house-salad"_s, u"cheeseburger"_s, u"soda"_s})
        REQUIRE(pos.addItem(item));
    QList<qint64> ids;
    for (const QVariant &v : pos.lines())
        ids << v.toMap()[u"id"_s].toLongLong();
    // Touch a line, then a bag: it goes in it.
    const auto put = [&](int line, int bag) {
        pos.selectLine(ids[line]);
        REQUIRE(pos.setSeat(bag));
    };
    put(0, 1);   // the burger and the water: bag 1
    put(1, 1);
    put(2, 2);   // the salad: bag 2
    put(3, 3);   // the cheeseburger: bag 3
    put(4, 4);   // the soda: bag 4
    // Then whose each bag is, and its break.
    REQUIRE(pos.setBag(1, u"Sam 3B"_s, 1));
    REQUIRE(pos.setBag(2, u"Sam 3B"_s, 2));
    REQUIRE(pos.setBag(3, u"Mia 1A"_s, 1));
    REQUIRE(pos.setBag(4, u"Mia 1A"_s, 2));
    return ids;
}

TEST_CASE("Tuckshop: the order rung in, then packed into bags", "[tuckshop]")
{
    app::PosShared shared(tuckshopData(), nullptr);
    app::PosService pos(&shared, u"Tuckshop"_s);

    // Rung in: everything is in no bag yet.
    REQUIRE(pos.addItem(u"classic-burger"_s));
    REQUIRE(pos.addItem(u"water"_s));
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/0/1"_s, u"Water/0/1"_s});
    CHECK(pos.checkInfo()[u"orderNumber"_s].toInt() == 1);
    CHECK_FALSE(pos.checkInfo()[u"touched"_s].toBool());
    CHECK_FALSE(pos.setCourse(2));   // a break is a bag's: none in hand
    const qint64 burger = pos.lines().at(0).toMap()[u"id"_s].toLongLong();
    const qint64 water = pos.lines().at(1).toMap()[u"id"_s].toLongLong();

    // A touched line goes in the bag touched next, and is let go.
    pos.selectLine(burger);
    CHECK(pos.checkInfo()[u"touched"_s].toBool());
    REQUIRE(pos.setSeat(1));
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/1"_s, u"Water/0/1"_s});
    CHECK_FALSE(pos.checkInfo()[u"touched"_s].toBool());
    CHECK(pos.checkInfo()[u"seat"_s].toInt() == 1);
    // The bag is a child's, for a break: what's in it is for that break.
    REQUIRE(pos.setBag(1, u"  Sam   3B "_s, 2));
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap() == QVariantMap{{u"1"_s, u"Sam 3B"_s}});
    CHECK(pos.checkInfo()[u"seatBreaks"_s].toMap() == QVariantMap{{u"1"_s, 2}});
    CHECK(linesOf(pos).at(0) == u"Classic Burger/1/2"_s);
    // What goes in it after is too; so is what's rung in with the bag in hand.
    pos.selectLine(water);
    REQUIRE(pos.setSeat(1));
    CHECK(linesOf(pos).at(1) == u"Water/1/2"_s);
    REQUIRE(pos.addItem(u"soda"_s));
    CHECK(linesOf(pos).at(2) == u"Soda/1/2"_s);
    // The break keys: the bag in hand's, everything in it.
    REQUIRE(pos.setCourse(1));
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/1"_s, u"Water/1/1"_s, u"Soda/1/1"_s});
    CHECK(pos.checkInfo()[u"seatBreaks"_s].toMap()[u"1"_s].toInt() == 1);
    REQUIRE(pos.setCourse(7));   // one of the day's: the last
    CHECK(pos.checkInfo()[u"seatBreaks"_s].toMap()[u"1"_s].toInt() == 2);

    // Another bag: its own break (the first until someone says); a line
    // moved to it takes that.
    REQUIRE(pos.setBag(1, u"Sam 3B"_s, 2));
    pos.selectLine(water);
    REQUIRE(pos.setSeat(2));
    CHECK(linesOf(pos).at(1) == u"Water/2/1"_s);
    // Out of every bag again.
    pos.selectLine(water);
    REQUIRE(pos.setSeat(0));
    CHECK(linesOf(pos).at(1) == u"Water/0/1"_s);

    // A break isn't a course to fire later: nothing is held.
    CHECK(pos.checkInfo()[u"heldCount"_s].toInt() == 0);
    for (const QVariant &v : pos.lines())
        CHECK_FALSE(v.toMap()[u"held"_s].toBool());

    // A name put right; an empty one is nobody's in particular again.
    REQUIRE(pos.setBag(1, u"Sam 3C"_s, 2));
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap()[u"1"_s].toString() == u"Sam 3C"_s);
    REQUIRE(pos.setBag(1, QString(), 2));
    CHECK_FALSE(pos.checkInfo()[u"seatNames"_s].toMap().contains(u"1"_s));
    CHECK_FALSE(pos.setBag(0, u"Nobody"_s, 1));
}

TEST_CASE("Tuckshop: two children, a bag each a break, paid once", "[tuckshop]")
{
    app::PosShared shared(tuckshopData(), nullptr);
    app::PosService pos(&shared, u"Tuckshop"_s);

    ringAndPack(pos);
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/1"_s, u"Water/1/1"_s, u"House Salad/2/2"_s,
                                     u"Cheeseburger/3/1"_s, u"Soda/4/2"_s});

    // Whose and when are the check's: saved with it.
    const core::Check &open = shared.open.begin()->second;
    const std::optional<core::Check> back = app::checkFromJson(app::toJson(open));
    REQUIRE(back);
    CHECK(back->seatNames == std::map<int, std::string>{{1, "Sam 3B"}, {2, "Sam 3B"}, {3, "Mia 1A"}, {4, "Mia 1A"}});
    CHECK(back->seatCourses == std::map<int, int>{{1, 1}, {2, 2}, {3, 1}, {4, 2}});
    CHECK(back->seatName(7).empty());
    CHECK(back->seatCourse(7) == 1);

    pos.openDrawerSession();
    REQUIRE(pos.tender(u"cash"_s));
    REQUIRE(pos.closeCheck());
    CHECK(shared.closedToday.back().seatNames.size() == 4);
    CHECK(shared.closedToday.back().lines.size() == 5);

    // Bags are a tuckshop's.
    app::PosData truck = test::seedPosData();
    truck.settings.serviceMode = "foodTruck";
    truck.settings.truckNoLogin = true;
    app::PosShared truckShared(truck, nullptr);
    app::PosService other(&truckShared, u"Truck"_s);
    REQUIRE(other.addItem(u"soda"_s));
    CHECK_FALSE(other.setBag(1, u"Sam"_s, 1));
}

TEST_CASE("Tuckshop: a slip a bag, everything in the bag on it", "[tuckshop][print]")
{
    QTemporaryDir dir;
    app::PosShared shared(tuckshopData(), nullptr);
    print::PrintSpooler spooler;
    print::TicketPrinter printer(spooler, dir.path());
    shared.printer = &printer;
    app::PosService pos(&shared, u"Tuckshop"_s);

    ringAndPack(pos);
    // One more bag nobody named, and something in no bag at all.
    REQUIRE(pos.setSeat(5));
    REQUIRE(pos.addItem(u"iced-tea"_s));
    REQUIRE(pos.setSeat(0));
    REQUIRE(pos.addItem(u"coffee"_s));
    pos.openDrawerSession();
    REQUIRE(pos.tender(u"cash"_s));
    REQUIRE(pos.closeCheck());   // paying sends it: every bag at once
    REQUIRE(spooler.waitIdle(5000));

    const QString kitchen = readFile(dir.filePath(u"kitchen.txt"_s));
    INFO(kitchen.toStdString());
    if (const QByteArray shots = qgetenv("VTM_SHOTS"); !shots.isEmpty()) {   // the slips, to look at
        QFile::remove(QString::fromLocal8Bit(shots) + u"/tuckshop-bag-slips.txt"_s);
        QFile::copy(dir.filePath(u"kitchen.txt"_s), QString::fromLocal8Bit(shots) + u"/tuckshop-bag-slips.txt"_s);
    }
    // What's in no bag first; then Little Lunch's bags, then Big Lunch's.
    const qsizetype noBag = kitchen.indexOf(u"1 Coffee"_s);
    const qsizetype samLittle = kitchen.indexOf(u"SAM 3B\n"_s);
    const qsizetype miaLittle = kitchen.indexOf(u"MIA 1A\n"_s);
    const qsizetype unnamed = kitchen.indexOf(u"BAG 5\n"_s);
    const qsizetype samBig = kitchen.indexOf(u"SAM 3B\n"_s, samLittle + 1);
    const qsizetype miaBig = kitchen.indexOf(u"MIA 1A\n"_s, miaLittle + 1);
    REQUIRE(noBag >= 0);
    REQUIRE(samLittle > noBag);
    REQUIRE(miaLittle > samLittle);
    REQUIRE(unnamed > miaLittle);     // (the first break, until someone says)
    REQUIRE(samBig > unnamed);
    REQUIRE(miaBig > samBig);
    CHECK(kitchen.count(u"LITTLE LUNCH"_s) == 3);
    CHECK(kitchen.count(u"BIG LUNCH"_s) == 2);
    CHECK(kitchen.count(u"KITCHEN"_s) == 6);   // six slips
    // What's in no bag says no child and no break.
    const QString first = kitchen.left(samLittle);
    CHECK_FALSE(first.contains(u"LUNCH"_s));
    // Each slip has its own bag's items, and only those.
    const QString sams = kitchen.mid(samLittle, miaLittle - samLittle);
    CHECK(sams.contains(u"LITTLE LUNCH"_s));
    CHECK(sams.contains(u"1 Classic Burger"_s));
    CHECK(sams.contains(u"1 Water"_s));          // the kitchen doesn't make it: in the bag all the same
    CHECK_FALSE(sams.contains(u"House Salad"_s));
    CHECK_FALSE(sams.contains(u"Cheeseburger"_s));
    const QString last = kitchen.mid(miaBig);
    CHECK(last.contains(u"BIG LUNCH"_s));
    CHECK(last.contains(u"1 Soda"_s));
    CHECK_FALSE(last.contains(u"Cheeseburger"_s));
    // The slip says whose and when at the top: not "S1" on every line, no "COURSE 2".
    CHECK_FALSE(kitchen.contains(u"S1 "_s));
    CHECK_FALSE(kitchen.contains(u"COURSE"_s));
    // Every one says the order it's part of.
    CHECK(kitchen.count(u"#1"_s) >= 6);
}

TEST_CASE("Tuckshop: a sale over the counter is a ticket as ever", "[tuckshop][print]")
{
    QTemporaryDir dir;
    app::PosShared shared(tuckshopData(), nullptr);
    print::PrintSpooler spooler;
    print::TicketPrinter printer(spooler, dir.path());
    shared.printer = &printer;
    app::PosService pos(&shared, u"Tuckshop"_s);

    // Nobody's bag: one ticket, what the kitchen makes on it.
    REQUIRE(pos.addItem(u"classic-burger"_s));
    REQUIRE(pos.addItem(u"water"_s));
    pos.openDrawerSession();
    REQUIRE(pos.tender(u"cash"_s));
    REQUIRE(pos.closeCheck());
    REQUIRE(spooler.waitIdle(5000));

    const QString kitchen = readFile(dir.filePath(u"kitchen.txt"_s));
    INFO(kitchen.toStdString());
    CHECK(kitchen.contains(u"1 Classic Burger"_s));
    CHECK_FALSE(kitchen.contains(u"Water"_s));
    CHECK_FALSE(kitchen.contains(u"LITTLE LUNCH"_s));
    CHECK(kitchen.count(u"KITCHEN"_s) == 1);
}

TEST_CASE("Tuckshop: a ticket's seat is its guest's name when someone said it", "[tuckshop][print]")
{
    // A restaurant's too: "Grandma" instead of "S2".
    const auto seed = test::seedPosData();
    core::Check c;
    c.id = 7;
    c.label = "Table 4";
    core::OrderLine l;
    l.id = 1;
    l.itemId = "house-salad";
    l.name = "House Salad";
    l.seat = 2;
    c.lines.push_back(l);
    l.id = 2;
    l.seat = 3;
    c.lines.push_back(l);
    c.seatNames[2] = "Grandma";
    const print::TicketContext ctx{seed.settings, [](std::int64_t) { return std::string("Sep 29, 2026 10:31 AM"); },
                                   [](std::int64_t) { return std::string("10:31 AM"); }, 0};
    const std::string k = print::renderText(print::kitchenTicket(c, c.lines, "Kitchen", false, ctx), 42);
    INFO(k);
    CHECK(k.find("Grandma 1 House Salad") != std::string::npos);
    CHECK(k.find("S3 1 House Salad") != std::string::npos);
}

TEST_CASE("Tuckshop: the breaks are the store's to name", "[tuckshop]")
{
    app::PosData d = tuckshopData();
    app::PosShared shared(d, nullptr);
    app::PosService pos(&shared, u"Tuckshop"_s);
    REQUIRE(pos.loginWithPin(u"1234"_s));   // the manager steps in

    QVariantMap store = pos.adminRecords(u"store"_s).value(0).toMap();
    CHECK(store[u"tuckshopBreaks"_s].toString() == u"Little Lunch\nBig Lunch"_s);
    store[u"tuckshopBreaks"_s] = u"Morning Tea\nFirst Break\n\nSecond Break"_s;
    REQUIRE(pos.adminSave(u"store"_s, 0, store));
    CHECK(shared.settings.tuckshopBreaks == std::vector<std::string>{"Morning Tea", "First Break", "Second Break"});
    CHECK(shared.settings.tuckshopBreak(2) == "First Break");
    CHECK(shared.settings.tuckshopBreak(4).empty());
    CHECK(pos.serviceInfo()[u"breaks"_s].toStringList().size() == 3);

    // None said: the usual two again. And they're saved with the settings.
    store[u"tuckshopBreaks"_s] = QString();
    REQUIRE(pos.adminSave(u"store"_s, 0, store));
    CHECK(shared.settings.tuckshopBreaks == std::vector<std::string>{"Little Lunch", "Big Lunch"});
    core::PosSettings named = shared.settings;
    named.tuckshopBreaks = {"Recess", "Lunch"};
    CHECK(app::settingsFromJson(app::toJson(named)).tuckshopBreaks == named.tuckshopBreaks);

    // The mode is one of the store's ways to serve, and a screen's.
    store[u"serviceMode"_s] = u"foodTruck"_s;
    REQUIRE(pos.adminSave(u"store"_s, 0, store));
    CHECK_FALSE(shared.settings.servesTuckshop());
    store[u"serviceMode"_s] = u"tuckshop"_s;
    REQUIRE(pos.adminSave(u"store"_s, 0, store));
    CHECK(shared.settings.servesTuckshop());
}

TEST_CASE("Tuckshop screen: rung in, packed into bags, each bag a child's for a break", "[tuckshop][ui]")
{
    auto l = test::loadTestLayout();
    REQUIRE(l);
    app::PosShared shared(tuckshopData(), nullptr);
    app::PosService pos(&shared, u"Tuckshop"_s);
    LayoutController c(*l);
    c.setMealPeriod(u"lunch"_s);
    c.setPos(&pos);
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{u"controller"_s, QVariant::fromValue(&c)}, {u"width"_s, 1600}, {u"height"_s, 900}});
    engine.loadFromModule("ViewTouch", "Main");
    REQUIRE_FALSE(engine.rootObjects().isEmpty());
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    REQUIRE(window);
    REQUIRE(QTest::qWaitForWindowExposed(window));
    QTest::qWait(50);
    QQuickItem *root = window->contentItem();
    const auto tap = [&](const QString &name) {
        QQuickItem *item = named(root, name);
        REQUIRE(item);
        QTest::mouseClick(window, Qt::LeftButton, {}, item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint());
        QTest::qWait(60);
    };
    const auto text = [&](const QString &name) {
        QQuickItem *item = named(root, name);
        return item ? item->property("text").toString() : u"(not on screen)"_s;
    };
    const auto shot = [&](const char *name) {   // VTM_SHOTS=<dir>: pictures of it, to look at
        if (const QByteArray dir = qgetenv("VTM_SHOTS"); !dir.isEmpty())
            window->grabWindow().save(QString::fromLocal8Bit(dir) + u'/' + QString::fromLatin1(name) + u".png"_s);
    };

    // The truck's order screen. Rung in first: it's all in no bag.
    CHECK(c.pageId() == u"index-lunch"_s);
    CHECK_FALSE(named(root, u"seatAdd"_s));            // nothing to pack yet
    REQUIRE(pos.addItem(u"classic-burger"_s));
    REQUIRE(pos.addItem(u"water"_s));
    REQUIRE(pos.addItem(u"house-salad"_s));
    QTest::qWait(60);
    CHECK(text(u"seat0"_s) == u"No bag ·3"_s);
    CHECK(text(u"seatAdd"_s) == u"+ Bag"_s);
    CHECK_FALSE(named(root, u"course1"_s));            // a break is a bag's: none in hand
    shot("tuckshop-0-rung-in");
    const auto id = [&](int line) { return pos.lines().at(line).toMap()[u"id"_s].toLongLong(); };

    // A touched line, then + Bag: a new bag with it in it.
    pos.selectLine(id(0));
    tap(u"seatAdd"_s);
    CHECK(linesOf(pos).at(0) == u"Classic Burger/1/1"_s);
    CHECK(text(u"seat1"_s) == u"Bag 1 LL ·1"_s);
    CHECK(text(u"seat0"_s) == u"No bag ·2"_s);
    // Another line, then that bag: in it too.
    pos.selectLine(id(1));
    tap(u"seat1"_s);
    CHECK(linesOf(pos).at(1) == u"Water/1/1"_s);
    CHECK(text(u"seat1"_s) == u"Bag 1 LL ·2"_s);
    CHECK_FALSE(named(root, u"whoField"_s));
    // The salad: a second bag.
    pos.selectLine(id(2));
    tap(u"seatAdd"_s);
    CHECK(linesOf(pos).at(2) == u"House Salad/2/1"_s);
    CHECK(text(u"seat0"_s) == u"No bag"_s);            // all packed
    shot("tuckshop-1-packed");

    // The bag in hand touched again: whose is it, and for which break?
    tap(u"seat2"_s);
    QQuickItem *field = named(root, u"whoField"_s);
    REQUIRE(field);
    CHECK(text(u"bagBreak1"_s) == u"Little Lunch"_s);
    CHECK(text(u"bagBreak2"_s) == u"Big Lunch"_s);
    field->setProperty("text", u"Sam 3B"_s);
    tap(u"bagBreak2"_s);
    shot("tuckshop-2-whose-bag");
    tap(u"whoSave"_s);
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap() == QVariantMap{{u"2"_s, u"Sam 3B"_s}});
    CHECK(pos.checkInfo()[u"seatBreaks"_s].toMap() == QVariantMap{{u"2"_s, 2}});
    CHECK(linesOf(pos).at(2) == u"House Salad/2/2"_s);
    CHECK(text(u"seat2"_s) == u"Sam 3B BL ·1"_s);

    // The first bag: touched (in hand), touched again (whose).
    tap(u"seat1"_s);
    CHECK(pos.checkInfo()[u"seat"_s].toInt() == 1);
    CHECK_FALSE(named(root, u"whoField"_s));
    tap(u"seat1"_s);
    REQUIRE(named(root, u"whoField"_s));
    named(root, u"whoField"_s)->setProperty("text", u"Sam 3B"_s);
    tap(u"whoSave"_s);
    CHECK(text(u"seat1"_s) == u"Sam 3B LL ·2"_s);

    // The break keys under the bags: the bag in hand's, at a touch.
    CHECK(text(u"course1"_s) == u"Little Lunch"_s);
    CHECK(text(u"course2"_s) == u"Big Lunch"_s);
    CHECK_FALSE(named(root, u"course3"_s));
    tap(u"course2"_s);
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/2"_s, u"Water/1/2"_s, u"House Salad/2/2"_s});
    tap(u"course1"_s);
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/1"_s, u"Water/1/1"_s, u"House Salad/2/2"_s});
    shot("tuckshop-3-two-bags");

    // No bag: over the counter; no break keys.
    tap(u"seat0"_s);
    CHECK(pos.checkInfo()[u"seat"_s].toInt() == 0);
    CHECK_FALSE(named(root, u"course1"_s));
}
