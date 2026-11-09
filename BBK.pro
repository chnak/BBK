TEMPLATE = app
TARGET = BBK

CONFIG += qt thread warn_on debug_and_release cascades

SOURCES += *.cpp
HEADERS += *.h

LIBS += -lbbplatform -lscreen -lbps -lbtapi -lbluetooth

INCLUDEPATH += $$[QT_INSTALL_HEADERS]/QtCore \
              $$[QT_INSTALL_HEADERS]/QtGui

device {
	CONFIG(release, debug|release) {
		DESTDIR = o.le-v7
		QMAKE_CXXFLAGS_RELEASE += -fno-strict-aliasing
	}
	CONFIG(debug, debug|release) {
		DESTDIR = o.le-v7-g
	}
}

simulator {
	CONFIG(release, debug|release) {
		DESTDIR = o
	}
	CONFIG(debug, debug|release) {
		DESTDIR = o-g
	}
}

OBJECTS_DIR = $${DESTDIR}/.obj
MOC_DIR = $${DESTDIR}/.moc
RCC_DIR = $${DESTDIR}/.rcc
UI_DIR = $${DESTDIR}/.ui

suredelete.target = surefclean
suredelete.commands = $(DEL_FILE) $${MOC_DIR}/*; $(DEL_FILE) $${RCC_DIR}/*; $(DEL_FILE) $${UI_DIR}/*
suredelete.depends = distclean

QMAKE_EXTRA_TARGETS += suredelete
