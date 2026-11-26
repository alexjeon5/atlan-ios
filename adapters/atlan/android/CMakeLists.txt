# SPDX-License-Identifier: GPL-2.0-or-later
# adapter: the Atlan app's native library for the Android host.
atlan_android_app(atlan_android.cpp)
target_link_libraries(atlanhost PRIVATE atlan_adapter)
