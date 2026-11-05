// Dalla cartella sim7080_serial_bridge:
//   g++ -std=c++17 -I. test/test_bridge_line.cpp -o /tmp/test_bridge_line && /tmp/test_bridge_line
#include <stdio.h>
#include <string.h>
#include <string>
#include "bridge_line.h"

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { printf("FAIL %s:%d %s\n", __func__, __LINE__, #cond); failures++; } } while (0)

// Ritorna l'ultimo evento e, con complete, quante righe sono state completate
static LineEvent feedAll(LineBuffer& b, const std::string& s, int* complete = nullptr, std::string* last = nullptr) {
  LineEvent e = LineEvent::None;
  for (char c : s) {
    e = b.feed(c);
    if (e == LineEvent::Complete) {
      if (complete) (*complete)++;
      if (last) *last = b.text();
      b.clear();
    }
  }
  return e;
}

static void testSimpleLine() {
  LineBuffer b;
  int n = 0; std::string last;
  feedAll(b, "AT+CSQ\r", &n, &last);
  CHECK(n == 1);
  CHECK(last == "AT+CSQ");
}

// Ogni terminale chiude la riga a modo suo: non devono partire comandi vuoti
static void testLineEndings() {
  for (const char* eol : {"\r", "\n", "\r\n", "\n\r"}) {
    LineBuffer b;
    int n = 0; std::string last;
    feedAll(b, std::string("AT") + eol + "ATI" + eol, &n, &last);
    CHECK(n == 2);
    CHECK(last == "ATI");
  }
}

static void testEmptyLinesIgnored() {
  LineBuffer b;
  CHECK(b.feed('\r') == LineEvent::None);
  CHECK(b.feed('\n') == LineEvent::None);
}

static void testBackspace() {
  for (char erase : {'\b', (char)0x7F}) {
    LineBuffer b;
    int n = 0; std::string last;
    CHECK(b.feed('A') == LineEvent::Stored);
    CHECK(b.feed('X') == LineEvent::Stored);
    CHECK(b.feed(erase) == LineEvent::Erased);
    feedAll(b, "T\r", &n, &last);
    CHECK(last == "AT");

    LineBuffer empty;
    CHECK(empty.feed(erase) == LineEvent::None);
  }
}

static void testControlCharactersDropped() {
  LineBuffer b;
  int n = 0; std::string last;
  feedAll(b, "A\x1b\x01T\xc3\r", &n, &last);
  CHECK(last == "AT");
}

// Il comando piu' lungo del firmware deve passare intero
static void testLongestLineAccepted() {
  LineBuffer b;
  std::string text = "AT+CCOAPPARA=" + std::string(BRIDGE_LINE_MAX - 13, 'x');
  int n = 0; std::string last;
  feedAll(b, text + "\r", &n, &last);
  CHECK(n == 1);
  CHECK(last == text);
  CHECK(last.size() == 559);
}

static void testTooLongDiscarded() {
  LineBuffer b;
  std::string text(BRIDGE_LINE_MAX + 1, 'x');
  int n = 0;
  CHECK(feedAll(b, text + "\r", &n) == LineEvent::TooLong);
  CHECK(n == 0);
  // Dopo lo scarto la riga successiva funziona
  std::string last;
  feedAll(b, "\nAT\r", &n, &last);
  CHECK(n == 1);
  CHECK(last == "AT");
}

static void testBackspaceIgnoredWhileDiscarding() {
  LineBuffer b;
  feedAll(b, std::string(BRIDGE_LINE_MAX + 5, 'x'));
  CHECK(b.feed('\b') == LineEvent::None);
  CHECK(b.feed('x') == LineEvent::None);
  CHECK(b.feed('\r') == LineEvent::TooLong);
}

int main() {
  testSimpleLine();
  testLineEndings();
  testEmptyLinesIgnored();
  testBackspace();
  testControlCharactersDropped();
  testLongestLineAccepted();
  testTooLongDiscarded();
  testBackspaceIgnoredWhileDiscarding();
  printf(failures ? "%d test falliti\n" : "tutti i test superati\n", failures);
  return failures ? 1 : 0;
}
