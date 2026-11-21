/*
 *  Copyright (C) 2026 Bongani Mbigi
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 2 or (at your option)
 *  version 3 of the License.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

// Linked into every unit test (add_unit_test): marks the process, and what it starts, as a test, before
// main() runs, so that SyncTokenStore's default store is an in-memory one and never the user's Keychain,
// Credential Manager or keyring (SyncTokenStore::refusesSystemStore). Kept free of the app's libraries so
// that every test can link it.

#include <cstdlib>

namespace
{
    // Must match SyncTokenStore::TestBinaryVariable
    bool mark()
    {
#ifdef _WIN32
        return _putenv_s("OPENVIREO_TEST_BINARY", "1") == 0;
#else
        return setenv("OPENVIREO_TEST_BINARY", "1", 1) == 0;
#endif
    }

    [[maybe_unused]] const bool marked = mark();
} // namespace
