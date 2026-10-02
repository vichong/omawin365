QMAKE ?= qmake6
PREFIX ?= $(HOME)/.local

.PHONY: all install test test-session test-session-public test-window clean
all:
	mkdir -p .build
	cd .build && $(QMAKE) ../omawin365.pro && $(MAKE)

install: all
	install -Dm755 .build/omawin365 "$(DESTDIR)$(PREFIX)/bin/omawin365"
	install -Dm644 assets/omawin365.desktop "$(DESTDIR)$(PREFIX)/share/applications/omawin365.desktop"
	install -Dm644 assets/omawin365.svg "$(DESTDIR)$(PREFIX)/share/icons/hicolor/scalable/apps/omawin365.svg"

# Test targets are independent of the application and never use real profiles.
test: test-session test-session-public test-window
	mkdir -p .build-tests/profiles
	cd .build-tests/profiles && $(QMAKE) ../../tests/profiles.pro && $(MAKE) && ./tst_profiles
	mkdir -p .build-tests/browser
	cd .build-tests/browser && $(QMAKE) ../../tests/browserauth.pro && $(MAKE) && ./browserauth_test
	mkdir -p .build-tests/transport
	cd .build-tests/transport && $(QMAKE) ../../tests/promptparser.pro && $(MAKE) && ./tst_promptparser

test-session:
	mkdir -p .build-tests/session
	cd .build-tests/session && $(QMAKE) ../../tests/session.pro && $(MAKE) && ./tst_session

test-session-public:
	mkdir -p .build-tests/session-public
	cd .build-tests/session-public && $(QMAKE) ../../tests/session_public.pro && $(MAKE) && ./tst_session_public

test-window:
	mkdir -p .build-tests/window
	cd .build-tests/window && $(QMAKE) ../../tests/window.pro && $(MAKE) && ./tst_window

clean:
	$(MAKE) -C .build clean
