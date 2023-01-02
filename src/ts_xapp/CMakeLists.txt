#==================================================================================
#	Copyright (c) 2020 AT&T Intellectual Property.
#
#   Licensed under the Apache License, Version 2.0 (the "License"),
#   you may not use this file except in compliance with the License.
#   You may obtain a copy of the License at
#
#       http://www.apache.org/licenses/LICENSE-2.0
#
#   Unless required by applicable law or agreed to in writing, software
#   distributed under the License is distributed on an "AS IS" BASIS,
#   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
#   See the License for the specific language governing permissions and
#   limitations under the License.
#==================================================================================
#

find_package(Protobuf REQUIRED)


add_executable( sla-spa ts_xapp.cpp )
#link_libraries(InfluxDB)
#find_package(InfluxDB) 
#ADD_LIBRARY(InfluxDB SHARED IMPORTED)
#SET_TARGET_PROPERTIES(InfluxDB PROPERTIES IMPORTED_LOCATION /usr/local/lib/libInfluxDB.so)

#set(CMAKE_PREFIX_PATH /usr/local/lib)
#find_library(InfluxDB libInfluxDB.so REQUIRED PATHS /usr/local/lib/libInfluxDB.so)
#find_package(InfluxDB CONFIG REQUIRED)
#target_include_directories(ts_xapp PRIVATE /usr/local/include)
target_link_libraries( sla-spa InfluxDB;ricxfcpp;rmr_si;pthread;curl;rc-api;grpc++;${Protobuf_LIBRARY};mdclog)

install(
    TARGETS sla-spa
    DESTINATION ${install_bin}
)

