#  Copyright (C) 2011 Felix Geyer <debfx@fobos.de>
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 2 or (at your option)
#  version 3 of the License.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.

include_directories(${CMAKE_CURRENT_SOURCE_DIR}/..)

add_unit_test(NAME testgui SOURCES TestGui.cpp ../util/TemporaryFile.cpp ../mock/MockRemoteProcess.cpp LIBS ${TEST_LIBRARIES})
add_unit_test(NAME testguipixmaps SOURCES TestGuiPixmaps.cpp LIBS ${TEST_LIBRARIES})
add_unit_test(NAME testguivault SOURCES TestGuiVault.cpp ../util/TemporaryFile.cpp LIBS ${TEST_LIBRARIES})
add_unit_test(NAME testguiautosync SOURCES TestGuiAutoSync.cpp ../util/TemporaryFile.cpp ../util/FakeAccountServer.cpp LIBS ${TEST_LIBRARIES})
add_unit_test(NAME testguiaccounts SOURCES TestGuiAccounts.cpp ../util/TemporaryFile.cpp ../util/FakeAccountServer.cpp LIBS ${TEST_LIBRARIES})

# Regenerates docs/images from the Vault Index layout. As a test it skips itself; build the
# docs_screenshots target to write the images (or run it with KPXC_DOCS_SCREENSHOT_DIR set).
add_unit_test(NAME testdocsscreenshots SOURCES TestDocsScreenshots.cpp LIBS testsupport ${TEST_LIBRARIES})
add_custom_target(docs_screenshots
        COMMAND ${CMAKE_COMMAND} -E env LANG=en_US.UTF-8 KPXC_DOCS_SCREENSHOT_DIR=${PROJECT_SOURCE_DIR}/docs/images
                $<TARGET_FILE:testdocsscreenshots>
        DEPENDS testdocsscreenshots
        WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}
        COMMENT "Regenerating documentation screenshots in docs/images"
        USES_TERMINAL)

file(GLOB_RECURSE ATTACHMENTS_TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/attachments/*.cpp)
add_unit_test(NAME testguiattachments SOURCES ${ATTACHMENTS_TEST_SOURCES} LIBS ${TEST_LIBRARIES})
include_directories(testguiattachments PRIVATE ${PROJECT_SOURCE_DIR}/src/gui/entry)

if(KPXC_FEATURE_BROWSER)
    add_unit_test(NAME testguibrowser SOURCES TestGuiBrowser.cpp ../util/TemporaryFile.cpp LIBS ${TEST_LIBRARIES})
endif()

if(KPXC_FEATURE_FDOSECRETS)
    add_unit_test(NAME testguifdosecrets
                  SOURCES TestGuiFdoSecrets.cpp ../util/TemporaryFile.cpp ../util/FdoSecretsProxy.cpp
                  LIBS ${TEST_LIBRARIES}
                  LAUNCHER dbus-run-session --config-file ${CMAKE_CURRENT_SOURCE_DIR}/../data/dbus/session.conf -- sh -c "exec ./$0")
endif()
