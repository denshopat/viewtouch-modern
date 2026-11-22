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
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointingDevice>
#include <functional>
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
// item goes in a bag on the Bags screen, and a bag is a child's for a
// break; every bag is a slip of its own.

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
    REQUIRE(pos.moveToBag({ids[0], ids[1]}, -1) == 1);   // the burger and the water: a new bag
    REQUIRE(pos.moveToBag({ids[2]}, -1) == 2);
    REQUIRE(pos.moveToBag({ids[3]}, -1) == 3);
    REQUIRE(pos.moveToBag({ids[4]}, -1) == 4);
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

    // Rung in as any order: nothing in a bag.
    REQUIRE(pos.addItem(u"classic-burger"_s));
    REQUIRE(pos.addItem(u"water"_s));
    pos.setNextQuantity(3);
    REQUIRE(pos.addItem(u"soda"_s));
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/0/1"_s, u"Water/0/1"_s, u"Soda/0/1"_s});
    CHECK(pos.lines().at(2).toMap()[u"quantity"_s].toInt() == 3);
    CHECK(pos.checkInfo()[u"orderNumber"_s].toInt() == 1);
    const auto id = [&](int line) { return pos.lines().at(line).toMap()[u"id"_s].toLongLong(); };
    const qint64 burger = id(0), water = id(1), sodas = id(2);

    // Two items into a new bag: bag 1, the first break until someone says.
    CHECK(pos.moveToBag({burger, water}, -1) == 1);
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/1"_s, u"Water/1/1"_s, u"Soda/0/1"_s});
    // Whose it is and its break; what's in it is for that break.
    REQUIRE(pos.setBag(1, u"  Sam   3B "_s, 2));
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap() == QVariantMap{{u"1"_s, u"Sam 3B"_s}});
    CHECK(pos.checkInfo()[u"seatBreaks"_s].toMap() == QVariantMap{{u"1"_s, 2}});
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/2"_s, u"Water/1/2"_s, u"Soda/0/1"_s});

    // Two of the three sodas into it: the line split, one left in no bag.
    CHECK(pos.moveToBag({sodas}, 1, 2) == 1);
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/2"_s, u"Water/1/2"_s, u"Soda/0/1"_s, u"Soda/1/2"_s});
    CHECK(pos.lines().at(2).toMap()[u"quantity"_s].toInt() == 1);
    CHECK(pos.lines().at(3).toMap()[u"quantity"_s].toInt() == 2);
    // The last one into another bag; that one's named, its break the first.
    CHECK(pos.moveToBag({sodas}, -1) == 2);
    REQUIRE(pos.setBag(2, u"Mia"_s, 1));
    CHECK(linesOf(pos).at(2) == u"Soda/2/1"_s);
    // A bag's break changed: everything in it.
    REQUIRE(pos.setBag(1, u"Sam 3B"_s, 1));
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/1"_s, u"Water/1/1"_s, u"Soda/2/1"_s, u"Soda/1/1"_s});
    REQUIRE(pos.setBag(1, u"Sam 3B"_s, 9));   // one of the day's: the last
    CHECK(pos.checkInfo()[u"seatBreaks"_s].toMap()[u"1"_s].toInt() == 2);
    // Out of the bags again.
    CHECK(pos.moveToBag({water}, 0) == 0);
    CHECK(linesOf(pos).at(1) == u"Water/0/1"_s);

    // A name put right, or gone ("Bag 2" again).
    REQUIRE(pos.setBag(2, u"Mia 1A"_s, 1));
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap()[u"2"_s].toString() == u"Mia 1A"_s);
    REQUIRE(pos.setBag(2, QString(), 1));
    CHECK_FALSE(pos.checkInfo()[u"seatNames"_s].toMap().contains(u"2"_s));
    CHECK_FALSE(pos.setBag(0, u"Nobody"_s, 1));
    CHECK(pos.moveToBag({}, 1) == -1);

    // An empty bag, made and gone; one with something in it stays.
    REQUIRE(pos.setBag(3, u"Spare"_s, 1));
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap().contains(u"3"_s));
    REQUIRE(pos.removeBag(3));
    CHECK_FALSE(pos.checkInfo()[u"seatNames"_s].toMap().contains(u"3"_s));
    CHECK_FALSE(pos.checkInfo()[u"seatBreaks"_s].toMap().contains(u"3"_s));
    CHECK_FALSE(pos.removeBag(1));

    // A break isn't a course to fire later: nothing is held.
    CHECK(pos.checkInfo()[u"heldCount"_s].toInt() == 0);
    for (const QVariant &v : pos.lines())
        CHECK_FALSE(v.toMap()[u"held"_s].toBool());
}

TEST_CASE("Tuckshop: two children, a bag each a break, paid once", "[tuckshop]")
{
    app::PosShared shared(tuckshopData(), nullptr);
    app::PosService pos(&shared, u"Tuckshop"_s);

    ringAndPack(pos);
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/1"_s, u"Water/1/1"_s, u"House Salad/2/2"_s,
                                     u"Cheeseburger/3/1"_s, u"Soda/4/2"_s});

    // Whose and when are the order's: saved with it.
    const core::Check &open = shared.open.begin()->second;
    const std::optional<core::Check> back = app::checkFromJson(app::toJson(open));
    REQUIRE(back);
    CHECK(back->seatNames == std::map<int, std::string>{{1, "Sam 3B"}, {2, "Sam 3B"}, {3, "Mia 1A"}, {4, "Mia 1A"}});
    CHECK(back->seatCourses == std::map<int, int>{{1, 1}, {2, 2}, {3, 1}, {4, 2}});
    CHECK(back->seatName(7).empty());
    CHECK(back->seatCourse(7) == 1);

    // Paid as any order, once.
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
    CHECK(other.moveToBag({other.lines().at(0).toMap()[u"id"_s]}, -1) == -1);
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
    REQUIRE(pos.addItem(u"iced-tea"_s));
    REQUIRE(pos.moveToBag({pos.lines().last().toMap()[u"id"_s]}, -1) == 5);
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
    CHECK_FALSE(kitchen.left(samLittle).contains(u"LUNCH"_s));
    const QString sams = kitchen.mid(samLittle, miaLittle - samLittle);
    CHECK(sams.contains(u"1 Classic Burger"_s));
    CHECK(sams.contains(u"1 Water"_s));          // the kitchen doesn't make it: in the bag all the same
    CHECK_FALSE(sams.contains(u"House Salad"_s));
    CHECK_FALSE(sams.contains(u"Cheeseburger"_s));
    const QString last = kitchen.mid(miaBig);
    CHECK(last.contains(u"BIG LUNCH"_s));
    CHECK(last.contains(u"1 Soda"_s));
    CHECK_FALSE(last.contains(u"Cheeseburger"_s));
    CHECK_FALSE(kitchen.contains(u"S1 "_s));
    CHECK_FALSE(kitchen.contains(u"COURSE"_s));
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

TEST_CASE("Tuckshop screen: the Bags screen, as a finger uses it", "[tuckshop][ui]")
{
    auto l = test::loadTestLayout();
    REQUIRE(l);
    app::PosShared shared(tuckshopData(), nullptr);
    app::PosService pos(&shared, u"Tuckshop"_s);
    LayoutController c(*l);
    c.setMealPeriod(u"lunch"_s);
    c.setPos(&pos);
    QQmlApplicationEngine engine;
    // The till's screen: 1920 × 1080, the screen's own keys.
    engine.setInitialProperties({{u"controller"_s, QVariant::fromValue(&c)}, {u"width"_s, 1920}, {u"height"_s, 1080},
                                 {u"touchKeyboard"_s, true}});
    engine.loadFromModule("ViewTouch", "Main");
    REQUIRE_FALSE(engine.rootObjects().isEmpty());
    auto *window = qobject_cast<QQuickWindow *>(engine.rootObjects().first());
    REQUIRE(window);
    REQUIRE(QTest::qWaitForWindowExposed(window));
    QTest::qWait(50);
    QQuickItem *root = window->contentItem();
    // Everything on screen, the popups' too (they're the overlay's children).
    const auto find = [&](const QString &name) -> QQuickItem * {
        if (QQuickItem *hit = named(root, name))
            return hit;
        for (QQuickItem *top : window->contentItem()->parentItem() ? window->contentItem()->parentItem()->childItems() : QList<QQuickItem *>())
            if (QQuickItem *hit = named(top, name))
                return hit;
        return nullptr;
    };
    // A finger's tap (the till is a touch screen), not a mouse's click: a
    // touch is offered to what's behind a key too.
    QPointingDevice *finger = QTest::createTouchDevice();
    const auto tapItem = [&](QQuickItem *item) {
        REQUIRE(item);
        const QPoint at = item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
        QTest::touchEvent(window, finger).press(0, at, window);
        QTest::qWait(30);
        QTest::touchEvent(window, finger).release(0, at, window);
        QTest::qWait(80);
    };
    const auto tap = [&](const QString &name) { INFO(name.toStdString()); tapItem(find(name)); };
    const auto text = [&](const QString &name) {
        QQuickItem *item = find(name);
        return item ? item->property("text").toString() : u"(not on screen)"_s;
    };
    const auto shot = [&](const char *name) {
        if (const QByteArray dir = qgetenv("VTM_SHOTS"); !dir.isEmpty())
            window->grabWindow().save(QString::fromLocal8Bit(dir) + u'/' + QString::fromLatin1(name) + u".png"_s);
    };
    // A key of the screen's keyboard: by what it says, or what it is ("enter").
    const std::function<QQuickItem *(QQuickItem *, const QString &)> key = [&](QQuickItem *from, const QString &k) -> QQuickItem * {
        for (QQuickItem *item : from->childItems()) {
            if (!item->isVisible())
                continue;
            if (item->property("modelData").toString() == k && item->property("modelData").typeId() == QMetaType::QString)
                return item;
            if (QQuickItem *hit = key(item, k))
                return hit;
        }
        return nullptr;
    };
    const auto type = [&](const QString &word) {
        QQuickItem *keys = find(u"screenKeyboard"_s);
        REQUIRE(keys);
        for (const QChar ch : word) {
            if (ch.isUpper() != keys->property("shifted").toBool())   // (a field's first letter is a capital already)
                tapItem(key(keys, u"shift"_s));
            INFO(QString(ch).toStdString());
            tapItem(key(keys, QString(ch).toLower()));
        }
    };
    const auto id = [&](int line) { return pos.lines().at(line).toMap()[u"id"_s].toLongLong(); };

    // The order rung in as any order; the order screen's Bags says how many to pack.
    for (const QString &item : {u"classic-burger"_s, u"water"_s, u"house-salad"_s, u"cheeseburger"_s})
        REQUIRE(pos.addItem(item));
    QTest::qWait(80);
    CHECK(text(u"who-bags"_s) == u"Bags (4 to pack)"_s);
    CHECK_FALSE(find(u"seatAdd"_s));   // no bag chips on the order screen
    tap(u"who-bags"_s);
    QTest::qWait(150);
    REQUIRE(find(u"bag0"_s));   // the screen's first box: what's in no bag
    CHECK_FALSE(find(u"bag1"_s));
    shot("bags-0-open");

    // The burger and the water: touched, then + New Bag with These.
    tap(u"bagItem-"_s + QString::number(id(0)));
    tap(u"bagItem-"_s + QString::number(id(1)));
    CHECK(text(u"bagNew"_s) == u"+ New Bag with These"_s);
    tap(u"bagNew"_s);
    CHECK(linesOf(pos) == QStringList{u"Classic Burger/1/1"_s, u"Water/1/1"_s, u"House Salad/0/1"_s, u"Cheeseburger/0/1"_s});
    REQUIRE(find(u"bag1"_s));
    CHECK(text(u"bagName1"_s) == u"Bag 1: touch to name"_s);

    // Its name, typed on the screen's keys; Save.
    tap(u"bagName1"_s);
    QTest::qWait(300);
    REQUIRE(find(u"bagNameField"_s));
    type(u"Sam 3B"_s.replace(u' ', QString()));   // (the keys' space is "space")
    CHECK(find(u"bagNameField"_s)->property("text").toString() == u"Sam3B"_s);
    shot("bags-1-naming");
    tap(u"bagNameSave"_s);
    QTest::qWait(150);
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap() == QVariantMap{{u"1"_s, u"Sam3B"_s}});
    CHECK(text(u"bagName1"_s) == u"Sam3B"_s);

    // Its break: Big Lunch. Everything in it is.
    tap(u"bag1Break2"_s);
    CHECK(pos.checkInfo()[u"seatBreaks"_s].toMap() == QVariantMap{{u"1"_s, 2}});
    CHECK(linesOf(pos).mid(0, 2) == QStringList{u"Classic Burger/1/2"_s, u"Water/1/2"_s});

    // An empty bag, named with the keys' Enter, then the salad put in it.
    tap(u"bagNew"_s);
    REQUIRE(find(u"bag2"_s));
    tap(u"bagName2"_s);
    QTest::qWait(300);
    type(u"Mia"_s);
    tapItem(key(find(u"screenKeyboard"_s), u"enter"_s));
    QTest::qWait(150);
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap() == QVariantMap{{u"1"_s, u"Sam3B"_s}, {u"2"_s, u"Mia"_s}});
    tap(u"bagItem-"_s + QString::number(id(2)));
    tap(u"bag2Put"_s);
    CHECK(linesOf(pos).at(2) == u"House Salad/2/1"_s);

    // The cheeseburger into Sam's; then moved to Mia's after all.
    tap(u"bagItem-"_s + QString::number(id(3)));
    tap(u"bag1Put"_s);
    CHECK(linesOf(pos).at(3) == u"Cheeseburger/1/2"_s);
    tap(u"bagItem-"_s + QString::number(id(3)));
    tap(u"bag2Put"_s);
    CHECK(linesOf(pos).at(3) == u"Cheeseburger/2/1"_s);
    // The names held through it all.
    CHECK(pos.checkInfo()[u"seatNames"_s].toMap() == QVariantMap{{u"1"_s, u"Sam3B"_s}, {u"2"_s, u"Mia"_s}});
    shot("bags-2-packed");

    // Three sodas: two to Sam, one to Mia (how many?).
    tap(u"bagsDone"_s);
    QTest::qWait(150);
    CHECK_FALSE(find(u"bag0"_s));
    pos.setNextQuantity(3);
    REQUIRE(pos.addItem(u"soda"_s));
    QTest::qWait(80);
    CHECK(text(u"who-bags"_s) == u"Bags (3 to pack)"_s);
    tap(u"who-bags"_s);
    QTest::qWait(150);
    tap(u"bagItem-"_s + QString::number(id(4)));
    tap(u"bag1Put"_s);
    QTest::qWait(150);
    REQUIRE(find(u"bagHowMany-1"_s));
    shot("bags-3-how-many");
    tap(u"bagHowMany-2"_s);
    CHECK(linesOf(pos).mid(4) == QStringList{u"Soda/0/1"_s, u"Soda/1/2"_s});
    tap(u"bagItem-"_s + QString::number(id(4)));
    tap(u"bag2Put"_s);
    CHECK(linesOf(pos).at(4) == u"Soda/2/1"_s);
    QTest::qWait(80);
    shot("bags-4-done");
    tap(u"bagsDone"_s);
    QTest::qWait(150);
    CHECK(text(u"who-bags"_s) == u"Bags"_s);
}
