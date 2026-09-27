#include "calc.hpp"

#include <gtest/gtest.h>

using namespace atrium::calc;
using namespace std::chrono;

namespace {

Rates rates() {
    Rates r;
    r.per_euro = {{"USD", 1.10}, {"GBP", 0.85}, {"JPY", 160.0}};
    return r;
}

// Sunday 27 September 2026, 16:00 UTC.
Context ctx(const Rates* r = nullptr) {
    Context c;
    c.now = sys_days{year{2026} / 9 / 27} + hours(16);
    c.zone = "UTC";
    c.rates = r;
    c.home_currency = "USD";
    return c;
}

std::string result(std::string_view q, const Context& c = ctx()) {
    auto a = evaluate(q, c);
    return a ? a->result : "<none>";
}

} // namespace

TEST(Calc, Format) {
    EXPECT_EQ(format(1234567.5), "1,234,567.5");
    EXPECT_EQ(format(0.1 + 0.2), "0.3");
    EXPECT_EQ(format(-42), "-42");
    EXPECT_EQ(format(1.0 / 3), "0.3333333333");
}

TEST(Calc, Arithmetic) {
    EXPECT_EQ(result("2 + 3 * 4"), "14");
    EXPECT_EQ(result("(2 + 3) * 4"), "20");
    EXPECT_EQ(result("2^10"), "1,024");
    EXPECT_EQ(result("sqrt(16) + 1"), "5");
    EXPECT_EQ(result("5!"), "120");
    EXPECT_EQ(result("10 mod 3"), "1");
    EXPECT_EQ(result("1,000 * 2"), "2,000");
    EXPECT_EQ(result("7 × 6"), "42");
    EXPECT_EQ(result("42"), "<none>");     // a lone number
    EXPECT_EQ(result("firefox"), "<none>"); // a word
    EXPECT_EQ(result("5 apples"), "<none>");
    EXPECT_EQ(result("1/0"), "<none>");
}

TEST(Calc, Percentages) {
    EXPECT_EQ(result("15% of 80"), "12");
    EXPECT_EQ(result("80 + 15%"), "92");
    EXPECT_EQ(result("80 - 25%"), "60");
    EXPECT_EQ(result("20% off 50"), "40");
    EXPECT_EQ(result("50 * 10%"), "5");
}

TEST(Calc, Units) {
    EXPECT_EQ(result("10 km to mi"), "6.213711922 mi");
    EXPECT_EQ(result("10kg + 500g"), "10,500 g");  // the last unit typed
    EXPECT_EQ(result("5 ft 3 in"), "5.25 ft");      // one quantity, in the leading unit
    EXPECT_EQ(result("100 km / 2 h to km/h"), "50 km/h");
    EXPECT_EQ(result("72 f in c"), "22.22222222°C");
    EXPECT_EQ(result("-40 c to f"), "-40°F");
    EXPECT_EQ(result("10 in in cm"), "25.4 cm");
    EXPECT_EQ(result("1 GiB to MB"), "1,073.741824 MB");
    EXPECT_EQ(result("5 m * 4 m"), "20 m²");
    EXPECT_EQ(result("5kg + 5"), "10 kg");
    EXPECT_EQ(result("5 ft"), "1.524 m");  // a bare quantity in the other system
    EXPECT_EQ(result("1 hr"), "60 min");
    EXPECT_EQ(result("5 kg + 3 m"), "<none>");
    EXPECT_EQ(result("10 km to kg"), "<none>");
}

TEST(Calc, Bases) {
    EXPECT_EQ(result("255 to hex"), "0xFF");
    EXPECT_EQ(result("10 in binary"), "0b1010");
    EXPECT_EQ(result("0xff"), "255");
    EXPECT_EQ(result("0xff + 1"), "256");
}

TEST(Calc, Money) {
    const Rates r = rates();
    const Context c = ctx(&r);
    EXPECT_EQ(result("10 eur to usd", c), "11.00 USD");
    EXPECT_EQ(result("$22 in eur", c), "20.00 EUR");
    EXPECT_EQ(result("20 eur", c), "22.00 USD");  // to the home currency
    EXPECT_EQ(result("€10 + $11", c), "22.00 USD");
    EXPECT_EQ(result("1000 yen to gbp", c), "5.31 GBP");
    EXPECT_EQ(result("10 eur to usd"), "<none>");  // no rates yet
}

TEST(Calc, Dates) {
    EXPECT_EQ(result("today + 3 weeks"), "18 October");
    EXPECT_EQ(evaluate("today + 3 weeks", ctx())->result_badge, "Sunday");
    EXPECT_EQ(result("2 weeks ago"), "13 September");
    EXPECT_EQ(result("3 days from now"), "30 September");
    EXPECT_EQ(result("days until 25 dec"), "89 days");
    EXPECT_EQ(result("days since 1 sep"), "26 days");
    EXPECT_EQ(result("hours until 9pm"), "5 hours");
    EXPECT_EQ(result("25 dec"), "25 December");
    EXPECT_EQ(evaluate("25 dec", ctx())->result_badge, "Friday");
    EXPECT_EQ(result("31.1.2027 + 1 month"), "28 February 2027");
    EXPECT_EQ(result("9am + 3"), "12:00 PM, 27 September");
    EXPECT_EQ(result("next friday"), "2 October");
    EXPECT_EQ(result("tomorrow"), "<none>");  // a search
    EXPECT_EQ(result("friday"), "<none>");
}

TEST(Calc, TimeZones) {
    EXPECT_EQ(result("time in tokyo"), "1:00 AM, tomorrow");
    EXPECT_EQ(evaluate("time in tokyo", ctx())->result_badge, "Tokyo");
    EXPECT_EQ(result("5pm london in new york"), "12:00 PM");
    EXPECT_EQ(result("11pm in tokyo"), "8:00 AM, next day");
    EXPECT_EQ(result("time in atlantis"), "<none>");
}

TEST(Calc, EcbRates) {
    const char* xml = R"(<gesmes:Envelope><Cube><Cube time='2026-09-25'>
        <Cube currency='USD' rate='1.1712'/><Cube currency='JPY' rate='173.40'/></Cube></Cube></gesmes:Envelope>)";
    auto r = parse_ecb(xml);
    ASSERT_TRUE(r);
    EXPECT_EQ(r->date, "2026-09-25");
    EXPECT_DOUBLE_EQ(r->per_euro.at("USD"), 1.1712);
    EXPECT_DOUBLE_EQ(r->per_euro.at("JPY"), 173.40);
}
