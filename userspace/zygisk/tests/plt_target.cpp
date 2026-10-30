extern "C" int fixture_value();
extern "C" int fixture_second();

extern "C" int fixture_call() { return fixture_value(); }
extern "C" int fixture_call_second() { return fixture_second(); }
