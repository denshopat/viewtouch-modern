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
// one for Big Lunch, and pays once; every bag is a slip of its own.

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

TEST_CASE("Tuckshop: an item is a child's, for a break", "[tuckshop]")
{
    app::PosShared shared(tuckshopData(), nullptr);
    app::PosService pos(&shared, u"Tuckshop"_s);

    // The first child starts the order: what's rung in next is theirs.
    REQUIRE(pos.nameSeat(1, u"  Sam   3B "_s));
    CHECK(pos.hasCheck());
    CHECK(pos.checkInfo()[u"orderNumber"_s].toInt() == 1);
    CHECK(pos.checkInfo()[u"seat"_s].toInt() == 1);
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap() == QVariantMap{{u"1"_s, u"Sam 3B"_s}});
    REQUIRE(pos.addItem(u"classic-burger"_s));
    REQUIRE(pos.setCourse(2));   // Big Lunch
    REQUIRE(pos.addItem(u"house-salad"_s));
    // The next child; the break stays the one touched last.
    REQUIRE(pos.nameSeat(2, u"Mia"_s));
    REQUIRE(pos.addItem(u"soda"_s));
    REQUIRE(pos.setCourse(1));
    REQUIRE(pos.addItem(u"cheeseburger"_s));
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/1"_s, u"House Salad/1/2"_s, u"Soda/2/2"_s, u"Cheeseburger/2/1"_s});

    // Any item moves: touch it, then the child, then the break.
    const qint64 salad = pos.lines().at(1).toMap()[u"id"_s].toLongLong();
    pos.selectLine(salad);
    REQUIRE(pos.setSeat(2));
    REQUIRE(pos.setCourse(1));
    CHECK(linesOf(pos).at(1) == u"House Salad/2/1"_s);

    // A break isn't a course to fire later: nothing is held.
    CHECK(pos.checkInfo()[u"heldCount"_s].toInt() == 0);
    for (const QVariant &v : pos.lines())
        CHECK_FALSE(v.toMap()[u"held"_s].toBool());

    // A name put right; an empty one is nobody's in particular again.
    REQUIRE(pos.nameSeat(2, u"Mia 1A"_s));
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap()[u"2"_s].toString() == u"Mia 1A"_s);
    REQUIRE(pos.nameSeat(2, QString()));
    CHECK_FALSE(pos.checkInfo()[u"seatNames"_s].toMap().contains(u"2"_s));
    CHECK_FALSE(pos.nameSeat(0, u"Nobody"_s));

    // The names are the check's: saved with it.
    REQUIRE(pos.nameSeat(2, u"Mia 1A"_s));
    const core::Check &open = shared.open.begin()->second;
    const std::optional<core::Check> back = app::checkFromJson(app::toJson(open));
    REQUIRE(back);
    CHECK(back->seatNames == std::map<int, std::string>{{1, "Sam 3B"}, {2, "Mia 1A"}});
    CHECK(back->seatName(1) == "Sam 3B");
    CHECK(back->seatName(7).empty());

    // Paid once, for every bag.
    pos.openDrawerSession();
    REQUIRE(pos.tender(u"cash"_s));
    REQUIRE(pos.closeCheck());
    CHECK(shared.closedToday.back().seatNames.size() == 2);
    CHECK(shared.closedToday.back().lines.size() == 4);
}

TEST_CASE("Tuckshop: a slip a bag, everything in the bag on it", "[tuckshop][print]")
{
    QTemporaryDir dir;
    app::PosShared shared(tuckshopData(), nullptr);
    print::PrintSpooler spooler;
    print::TicketPrinter printer(spooler, dir.path());
    shared.printer = &printer;
    app::PosService pos(&shared, u"Tuckshop"_s);

    REQUIRE(pos.nameSeat(1, u"Sam 3B"_s));
    REQUIRE(pos.addItem(u"classic-burger"_s));
    REQUIRE(pos.addItem(u"water"_s));          // the kitchen doesn't make it: in the bag all the same
    REQUIRE(pos.setCourse(2));
    REQUIRE(pos.addItem(u"house-salad"_s));
    REQUIRE(pos.nameSeat(2, u"Mia 1A"_s));
    REQUIRE(pos.setCourse(1));
    REQUIRE(pos.addItem(u"cheeseburger"_s));
    REQUIRE(pos.setCourse(2));
    REQUIRE(pos.addItem(u"soda"_s));
    pos.openDrawerSession();
    REQUIRE(pos.tender(u"cash"_s));
    REQUIRE(pos.closeCheck());   // paying sends it: every bag at once
    REQUIRE(spooler.waitIdle(5000));

    const QString kitchen = readFile(dir.filePath(u"kitchen.txt"_s));
    INFO(kitchen.toStdString());
    if (const QByteArray shots = qgetenv("VTM_SHOTS"); !shots.isEmpty())   // the slips, to look at
        QFile::copy(dir.filePath(u"kitchen.txt"_s), QString::fromLocal8Bit(shots) + u"/tuckshop-bag-slips.txt"_s);
    // Four bags: Little Lunch's first, a child after the other.
    const qsizetype samLittle = kitchen.indexOf(u"SAM 3B\n"_s);
    const qsizetype miaLittle = kitchen.indexOf(u"MIA 1A\n"_s);
    const qsizetype samBig = kitchen.indexOf(u"SAM 3B\n"_s, samLittle + 1);
    const qsizetype miaBig = kitchen.indexOf(u"MIA 1A\n"_s, miaLittle + 1);
    REQUIRE(samLittle >= 0);
    REQUIRE(miaLittle > samLittle);
    REQUIRE(samBig > miaLittle);
    REQUIRE(miaBig > samBig);
    CHECK(kitchen.count(u"LITTLE LUNCH"_s) == 2);
    CHECK(kitchen.count(u"BIG LUNCH"_s) == 2);
    // Each slip has its own bag's items, and only those.
    const QString first = kitchen.mid(samLittle, miaLittle - samLittle);
    CHECK(first.contains(u"LITTLE LUNCH"_s));
    CHECK(first.contains(u"1 Classic Burger"_s));
    CHECK(first.contains(u"1 Water"_s));
    CHECK_FALSE(first.contains(u"House Salad"_s));
    CHECK_FALSE(first.contains(u"Cheeseburger"_s));
    const QString last = kitchen.mid(miaBig);
    CHECK(last.contains(u"BIG LUNCH"_s));
    CHECK(last.contains(u"1 Soda"_s));
    CHECK_FALSE(last.contains(u"Cheeseburger"_s));
    // The slip says whose and when at the top: not "S1" on every line, no "COURSE 2".
    CHECK_FALSE(kitchen.contains(u"S1 "_s));
    CHECK_FALSE(kitchen.contains(u"COURSE"_s));
    // Every one says the order it's part of.
    CHECK(kitchen.count(u"#1"_s) >= 4);
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

TEST_CASE("Tuckshop screen: the children on the chips, the breaks by name", "[tuckshop][ui]")
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

    // The truck's order screen, and before there's an order: + Child.
    CHECK(c.pageId() == u"index-lunch"_s);
    CHECK_FALSE(pos.hasCheck());
    CHECK(text(u"seatAdd"_s) == u"+ Child"_s);
    CHECK(text(u"seat0"_s) == u"No bag"_s);
    shot("tuckshop-0-start");

    // + Child asks whose bag it is; saved, the order starts and it's theirs.
    tap(u"seatAdd"_s);
    QQuickItem *field = named(root, u"whoField"_s);
    REQUIRE(field);
    shot("tuckshop-1-whose-bag");
    field->setProperty("text", u"Sam 3B"_s);
    tap(u"whoSave"_s);
    REQUIRE(pos.hasCheck());
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap() == QVariantMap{{u"1"_s, u"Sam 3B"_s}});
    CHECK(pos.checkInfo()[u"seat"_s].toInt() == 1);
    CHECK(text(u"seat1"_s) == u"Sam 3B"_s);

    // The breaks, by name: Little Lunch to start with.
    CHECK(text(u"course1"_s) == u"Little Lunch"_s);
    CHECK(text(u"course2"_s) == u"Big Lunch"_s);
    CHECK_FALSE(named(root, u"course3"_s));
    REQUIRE(pos.addItem(u"classic-burger"_s));
    tap(u"course2"_s);
    CHECK(pos.checkInfo()[u"course"_s].toInt() == 2);
    REQUIRE(pos.addItem(u"house-salad"_s));
    QTest::qWait(60);
    CHECK(text(u"seat1"_s) == u"Sam 3B ·2"_s);   // (how many are theirs)

    // The next child.
    tap(u"seatAdd"_s);
    REQUIRE(named(root, u"whoField"_s));
    named(root, u"whoField"_s)->setProperty("text", u"Mia 1A"_s);
    tap(u"whoSave"_s);
    CHECK(pos.checkInfo()[u"seat"_s].toInt() == 2);
    REQUIRE(pos.addItem(u"soda"_s));
    tap(u"course1"_s);
    REQUIRE(pos.addItem(u"cheeseburger"_s));
    QTest::qWait(60);
    CHECK(text(u"seat2"_s) == u"Mia 1A ·2"_s);
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/1"_s, u"House Salad/1/2"_s, u"Soda/2/2"_s, u"Cheeseburger/2/1"_s});
    shot("tuckshop-2-two-children");

    // A child's chip: what's rung in next is theirs again.
    tap(u"seat1"_s);
    CHECK(pos.checkInfo()[u"seat"_s].toInt() == 1);
    // Nobody's bag: over the counter.
    tap(u"seat0"_s);
    CHECK(pos.checkInfo()[u"seat"_s].toInt() == 0);
}
