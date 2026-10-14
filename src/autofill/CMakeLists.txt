#  Copyright (C) 2026 KeePassXC Team <team@keepassxc.org>
#
#  This program is free software: you can redistribute it and/or modify
#  it under the terms of the GNU General Public License as published by
#  the Free Software Foundation, either version 3 of the License, or
#  (at your option) any later version.
#
#  This program is distributed in the hope that it will be useful,
#  but WITHOUT ANY WARRANTY; without even the implied warranty of
#  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
#  GNU General Public License for more details.
#
#  You should have received a copy of the GNU General Public License
#  along with this program.  If not, see <http://www.gnu.org/licenses/>.

if(KPXC_FEATURE_AUTOFILL)
    set(AUTOFILL_EXTENSION_NAME KeePassXCAutofill)
    set(AUTOFILL_EXTENSION_IDENTIFIER "${MACOS_BUNDLE_IDENTIFIER}.autofill")
    set(AUTOFILL_APP_GROUP "${MACOS_TEAM_IDENTIFIER}.${MACOS_BUNDLE_IDENTIFIER}")
    # The identity store and the provider extension point arrived in macOS 11
    set(AUTOFILL_DEPLOYMENT_TARGET 11.0)
    # Apple Development and Developer ID certificates of the team both satisfy it
    set(AUTOFILL_PEER_REQUIREMENT
        "identifier \"${AUTOFILL_EXTENSION_IDENTIFIER}\" and anchor apple generic and certificate leaf[subject.OU] = \"${MACOS_TEAM_IDENTIFIER}\"")
    string(REPLACE "\"" "\\\"" AUTOFILL_PEER_REQUIREMENT_ESCAPED "${AUTOFILL_PEER_REQUIREMENT}")
    configure_file(autofill-config.h.in ${CMAKE_CURRENT_BINARY_DIR}/autofill-config.h @ONLY)

    # Provider service in the main app
    set(autofill_SOURCES
            AutofillCredentials.cpp
            AutofillHost.cpp
            AutofillPlatform.mm
            AutofillProtocol.cpp
            AutofillService.cpp
            AutofillSettingsPage.cpp
            AutofillSettingsWidget.cpp)
    set_source_files_properties(AutofillPlatform.mm PROPERTIES COMPILE_FLAGS "-fobjc-arc -Wno-old-style-cast")

    add_library(autofill STATIC ${autofill_SOURCES})
    target_include_directories(autofill PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_link_libraries(autofill
            browser
            Qt6::Core
            Qt6::Network
            Qt6::Widgets
            FluentQt::FluentQt
            "-framework AppKit"
            "-framework Security"
            # Weak so that the app still launches on macOS 10.15
            "-weak_framework AuthenticationServices")

    # The extension, built as KeePassXCAutofill.appex
    set(AUTOFILL_EXTENSION_DIR ${CMAKE_CURRENT_BINARY_DIR}/extension)
    configure_file(${CMAKE_SOURCE_DIR}/share/macosx/autofill/Info.plist.in ${AUTOFILL_EXTENSION_DIR}/Info.plist @ONLY)
    configure_file(${CMAKE_SOURCE_DIR}/share/macosx/autofill/KeePassXCAutofill.entitlements.in
                   ${AUTOFILL_EXTENSION_DIR}/KeePassXCAutofill.entitlements @ONLY)
    configure_file(${CMAKE_SOURCE_DIR}/share/macosx/autofill/keepassxc.entitlements.in
                   ${CMAKE_CURRENT_BINARY_DIR}/keepassxc.entitlements @ONLY)

    add_executable(${AUTOFILL_EXTENSION_NAME} MACOSX_BUNDLE
            extension/KPXAutofillClient.m
            extension/KPXCredentialProviderViewController.m)
    set_target_properties(${AUTOFILL_EXTENSION_NAME} PROPERTIES
            BUNDLE_EXTENSION appex
            MACOSX_BUNDLE_INFO_PLIST ${AUTOFILL_EXTENSION_DIR}/Info.plist
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS ${AUTOFILL_EXTENSION_DIR}/KeePassXCAutofill.entitlements)
    target_include_directories(${AUTOFILL_EXTENSION_NAME} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
    target_compile_options(${AUTOFILL_EXTENSION_NAME} PRIVATE -fobjc-arc -fapplication-extension)
    # App extensions have no main(); Foundation's NSExtensionMain runs the principal class
    target_link_options(${AUTOFILL_EXTENSION_NAME} PRIVATE -fapplication-extension "LINKER:-e,_NSExtensionMain")
    target_link_libraries(${AUTOFILL_EXTENSION_NAME}
            "-framework Foundation"
            "-framework AppKit"
            "-framework AuthenticationServices")

    # Profiles from the developer account, with the AutoFill entitlement and the App Group
    set(AUTOFILL_EXTENSION_PROFILE "${CMAKE_SOURCE_DIR}/share/macosx/autofill/KeePassXCAutofill.provisionprofile")
    set(AUTOFILL_APP_PROFILE "${CMAKE_SOURCE_DIR}/share/macosx/autofill/KeePassXC.provisionprofile")
    if(NOT EXISTS ${AUTOFILL_EXTENSION_PROFILE} OR NOT EXISTS ${AUTOFILL_APP_PROFILE})
        message(WARNING "AutoFill: share/macosx/autofill/ lacks KeePassXC.provisionprofile or "
                        "KeePassXCAutofill.provisionprofile. Signed builds won't launch or register the extension "
                        "until both are issued with the AutoFill entitlement and the ${AUTOFILL_APP_GROUP} App Group.")
    endif()
    if(EXISTS ${AUTOFILL_EXTENSION_PROFILE})
        add_custom_command(TARGET ${AUTOFILL_EXTENSION_NAME} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy ${AUTOFILL_EXTENSION_PROFILE}
                        $<TARGET_BUNDLE_CONTENT_DIR:${AUTOFILL_EXTENSION_NAME}>/embedded.provisionprofile)
    endif()

    # Handed to src/CMakeLists.txt, which embeds and signs the extension in the app bundle
    set(AUTOFILL_EXTENSION_NAME ${AUTOFILL_EXTENSION_NAME} PARENT_SCOPE)
    set(AUTOFILL_EXTENSION_ENTITLEMENTS ${AUTOFILL_EXTENSION_DIR}/KeePassXCAutofill.entitlements PARENT_SCOPE)
    set(AUTOFILL_APP_ENTITLEMENTS ${CMAKE_CURRENT_BINARY_DIR}/keepassxc.entitlements PARENT_SCOPE)
    set(AUTOFILL_APP_PROFILE ${AUTOFILL_APP_PROFILE} PARENT_SCOPE)
endif()
