// vi: ts=4 sw=4 noet:
/*
==================================================================================
	Copyright (c) 2020 AT&T Intellectual Property.

   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
==================================================================================
*/

/*
	Mnemonic:	ts_xapp.cpp
	Abstract:	Traffic Steering xApp
	           1. Receives A1 Policy
			       2. Receives anomaly detection
			       3. Requests prediction for UE throughput on current and neighbor cells
			       4. Receives prediction
			       5. Optionally exercises Traffic Steering action over E2

	Date:     22 April 2020
	Author:		Ron Shacham

  Modified: 21 May 2021 (Alexandre Huff)
            Update for traffic steering use case in release D.
            07 Dec 2021 (Alexandre Huff)
            Update for traffic steering use case in release E.
*/
  


#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <thread>
#include <iostream>
#include <memory>

#include <set>
#include <map>
#include <vector>
#include <string>
#include <unordered_map>

#include <rapidjson/document.h>
#include <rapidjson/writer.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/schema.h>
#include <rapidjson/reader.h>
#include <rapidjson/prettywriter.h>

#include <curl/curl.h>
#include <rmr/RIC_message_types.h>
#include "ricxfcpp/xapp.hpp"
#include "ricxfcpp/config.hpp"
#include <sstream>


/*
  FIXME unfortunately this RMR flag has to be disabled
  due to name resolution conflicts.
  RC xApp defines the same name for gRPC control messages.
*/
#undef RIC_CONTROL_ACK

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>
#include "../../ext/protobuf/api.grpc.pb.h"

//----------------------------------------------------------------------
// Ken creates the Route for InfluxDB
// Import InfluxDB Library, InfluxDBFactory
// Using the source code from offa InfluxDB C++ client library
//----------------------------------------------------------------------
#include <InfluxDBFactory.h>

//----------------------------------------------------------------------
// Ken imports Import c++ std Library below
//----------------------------------------------------------------------
#include <math.h>
#include <fstream>
#include <random>
#include <time.h>
#include <chrono>

//----------------------------------------------------------------------
// Ken improts MDC Log Library
//----------------------------------------------------------------------
#include <mdclog/mdclog.h>


using namespace rapidjson;
using namespace std;
using namespace xapp;

using Namespace = std::string;
using Key = std::string;
using Data = std::vector<uint8_t>;
using DataMap = std::map<Key, Data>;
using Keys = std::set<Key>;


//----------------------------------------------------------------------
// Ken assigns a variable to get the return address of influxdb
//----------------------------------------------------------------------
string influxdb_url = "http://ricplt-influxdb.ricplt:8086?db=UEData";
auto db_influx = influxdb::InfluxDBFactory::Get(influxdb_url);

std::unique_ptr<Xapp> xfw;
std::unique_ptr<api::MsgComm::Stub> rc_stub;

int rsrp_threshold = 0;

// scoped enum to identify which API is used to send control messages
enum class TsControlApi { REST, gRPC };
TsControlApi ts_control_api;  // api to send control messages
string ts_control_ep;         // api target endpoint

/* struct UEData {
  string serving_cell;
  int serving_cell_rsrp;
}; */

struct PolicyHandler : public BaseReaderHandler<UTF8<>, PolicyHandler> {
  unordered_map<string, string> cell_pred;
  std::string ue_id;
  bool ue_id_found = false;
  string curr_key = "";
  string curr_value = "";
  int policy_type_id;
  int policy_instance_id;
  int threshold;
  std::string operation;
  bool found_threshold = false;

  bool Null() { return true; }
  bool Bool(bool b) { return true; }
  bool Int(int i) {

    if (curr_key.compare("policy_type_id") == 0) {
      policy_type_id = i;
    } else if (curr_key.compare("policy_instance_id") == 0) {
      policy_instance_id = i;
    } else if (curr_key.compare("threshold") == 0) {
      found_threshold = true;
      threshold = i;
    }

    return true;
  }
  bool Uint(unsigned u) {

    if (curr_key.compare("policy_type_id") == 0) {
      policy_type_id = u;
    } else if (curr_key.compare("policy_instance_id") == 0) {
      policy_instance_id = u;
    } else if (curr_key.compare("threshold") == 0) {
      found_threshold = true;
      threshold = u;
    }

    return true;
  }
  bool Int64(int64_t i) {  return true; }
  bool Uint64(uint64_t u) {  return true; }
  bool Double(double d) {  return true; }
  bool String(const char* str, SizeType length, bool copy) {

    if (curr_key.compare("operation") != 0) {
      operation = str;
    }

    return true;
  }
  bool StartObject() {

    return true;
  }
  bool Key(const char* str, SizeType length, bool copy) {

    curr_key = str;

    return true;
  }
  bool EndObject(SizeType memberCount) {  return true; }
  bool StartArray() {  return true; }
  bool EndArray(SizeType elementCount) {  return true; }

};

struct PredictionHandler : public BaseReaderHandler<UTF8<>, PredictionHandler> {
  unordered_map<string, int> cell_pred_down;
  unordered_map<string, int> cell_pred_up;
  std::string ue_id;
  bool ue_id_found = false;
  string curr_key = "";
  string curr_value = "";
  string serving_cell_id;
  bool down_val = true;
  bool Null() {  return true; }
  bool Bool(bool b) {  return true; }
  bool Int(int i) {  return true; }
  bool Uint(unsigned u) {
    // Currently, we assume the first cell in the prediction message is the serving cell
    if ( serving_cell_id.empty() ) {
      serving_cell_id = curr_key;
    }

    if (down_val) {
      cell_pred_down[curr_key] = u;
      down_val = false;
    } else {
      cell_pred_up[curr_key] = u;
      down_val = true;
    }

    return true;

  }
  bool Int64(int64_t i) {  return true; }
  bool Uint64(uint64_t u) {  return true; }
  bool Double(double d) {  return true; }
  bool String(const char* str, SizeType length, bool copy) {

    return true;
  }
  bool StartObject() {  return true; }
  bool Key(const char* str, SizeType length, bool copy) {
    if (!ue_id_found) {

      ue_id = str;
      ue_id_found = true;
    } else {
      curr_key = str;
    }
    return true;
  }
  bool EndObject(SizeType memberCount) {  return true; }
  bool StartArray() {  return true; }
  bool EndArray(SizeType elementCount) {  return true; }
};

struct AnomalyHandler : public BaseReaderHandler<UTF8<>, AnomalyHandler> {
  /*
    Assuming we receive the following payload from AD
    [{"du-id": 1010, "ue-id": "Train passenger 2", "measTimeStampRf": 1620835470108, "Degradation": "RSRP RSSINR"}]
  */
  vector<string> prediction_ues;
  string curr_key = "";

  bool Key(const Ch* str, SizeType len, bool copy) {
    curr_key = str;
    return true;
  }

  bool String(const Ch* str, SizeType len, bool copy) {
    // We are only interested in the "ue-id"
    if ( curr_key.compare( "ue-id") == 0 ) {
      prediction_ues.push_back( str );
    }
    return true;
  }
};


/* struct UEDataHandler : public BaseReaderHandler<UTF8<>, UEDataHandler> {
  unordered_map<string, string> cell_pred;
  std::string serving_cell_id;
  int serving_cell_rsrp;
  int serving_cell_rsrq;
  int serving_cell_sinr;
  bool in_serving_array = false;
  int rf_meas_index = 0;

  bool in_serving_report_object = false;

  string curr_key = "";
  string curr_value = "";
  bool Null() { return true; }
  bool Bool(bool b) { return true; }
  bool Int(int i) {

    return true;
  }

  bool Uint(unsigned i) {

    if (in_serving_report_object) {
      if (curr_key.compare("rsrp") == 0) {
	serving_cell_rsrp = i;
      } else if (curr_key.compare("rsrq") == 0) {
	serving_cell_rsrq = i;
      } else if (curr_key.compare("rssinr") == 0) {
	serving_cell_sinr = i;
      }
    }

    return true; }
  bool Int64(int64_t i) {

    return true; }
  bool Uint64(uint64_t i) {

    return true; }
  bool Double(double d) { return true; }
  bool String(const char* str, SizeType length, bool copy) {

    if (curr_key.compare("ServingCellID") == 0) {
      serving_cell_id = str;
    }

    return true;
  }
  bool StartObject() {
    if (curr_key.compare("ServingCellRF") == 0) {
      in_serving_report_object = true;
    }

    return true; }
  bool Key(const char* str, SizeType length, bool copy) {

    curr_key = str;
    return true;
  }
  bool EndObject(SizeType memberCount) {
    if (curr_key.compare("ServingCellRF") == 0) {
      in_serving_report_object = false;
    }
    return true; }
  bool StartArray() {

    if (curr_key.compare("ServingCellRF") == 0) {
      in_serving_array = true;
    }

    return true;
  }
  bool EndArray(SizeType elementCount) {

    if (curr_key.compare("servingCellRF") == 0) {
      in_serving_array = false;
      rf_meas_index = 0;
    }

    return true; }
}; */


/* unordered_map<string, UEData> get_sdl_ue_data() {

  fprintf(stderr, "In get_sdl_ue_data()\n");

  unordered_map<string, string> ue_data;

  unordered_map<string, UEData> return_ue_data_map;

  std::string prefix3="";
  Keys K2 = sdl->findKeys(nsu, prefix3);
  DataMap Dk2 = sdl->get(nsu, K2);

  string ue_json;
  string ue_id;

  for(auto si=K2.begin();si!=K2.end();++si){
    std::vector<uint8_t> val_v = Dk2[(*si)]; // 4 lines to unpack a string
    char val[val_v.size()+1];                               // from Data
    int i;

    for(i=0;i<val_v.size();++i) val[i] = (char)(val_v[i]);
    val[i]='\0';
      ue_id.assign((std::string)*si);

      ue_json.assign(val);
      ue_data[ue_id] =  ue_json;
  }

  for (auto map_iter = ue_data.begin(); map_iter != ue_data.end(); map_iter++) {
    UEDataHandler handler;
    Reader reader;
    StringStream ss(map_iter->second.c_str());
    reader.Parse(ss,handler);

    string ueID = map_iter->first;
    string serving_cell_id = handler.serving_cell_id;
    int serv_rsrp = handler.serving_cell_rsrp;

    return_ue_data_map[ueID] = {serving_cell_id, serv_rsrp};

  }

  return return_ue_data_map;
} */

//----------------------------------------------------------------------
// Ken defines state transition type for updating cell data 
//----------------------------------------------------------------------
typedef enum{
    stay,
    switch_to_nr,
    handover_ue_k2
}Updata_scenario;

//----------------------------------------------------------------------
// Ken defines Slice 
//----------------------------------------------------------------------
typedef enum{
    embb,
    mmtc,
    urllc
}Scenario;
static unordered_map<string, Scenario> const Scenario_table = { {"embb", Scenario::embb} , {"mmtc", Scenario::mmtc} , {"urllc", Scenario::urllc} };


//----------------------------------------------------------------------
// Ken defines UE 
//----------------------------------------------------------------------
struct UE
{
    std::string Name;
    int X;
    int Y;
    std::string Serv_cell; // the cell currently serving a instance
    std::string HO_cell; // the previous cell serves a instance
    std::unordered_map<std::string, float> serv_slice_utilization ; // (A Slice PRB/Total PRB) of the cell currently serving a instance
    std::vector<std::string> NR_cell; // neighbor cells of a instance 
    
    // (A Slice PRB/Total PRB) of the neighbors cell currently serving the instance
    std::vector< std::unordered_map<std::string, float>> NR_Slice_utilization; 

    std::unordered_map< std::string , int> Slice_prb_req; // Demanded Slice PRB a instance require to a cell
    std::unordered_map< std::string , int> Slice_prb_used; // Slice PRB a instance use in a cell

    std::vector<std::string> Slice_req; // Demanded Slice PRB a instance require
    std::vector<std::string> Slice_used; // Slice PRB a instance use
  
    UE(){};
    UE(std::string name, std::string serv_cell, std::vector<std::string> nr_cell) {
        Name = name;
        Serv_cell = serv_cell;
        NR_cell = nr_cell;
    }
    void Set_demand_slice(std::string slice_prb_req){
        Slice_req.push_back(slice_prb_req);
    }    
    std::vector<std::string>  Get_demand_slice(void){    
        for(const auto& s : Slice_prb_req){
            Slice_req.push_back(s.first);
        } 
        return Slice_req;
    }
    void Set_used_slice(std::string slice_used){
        Slice_used.push_back(slice_used);
    }
    std::vector<std::string>  Get_used_slice(void){    
        return Slice_used;
    }
};

//----------------------------------------------------------------------
// Ken defines Cell 
//----------------------------------------------------------------------
struct Cell{
    std::string Name;
    int X;
    int Y;
    int Cell_prb_avail;    // Total PRB a instance can provide
    
    std::unordered_map< std::string , int> Slice_prb_avail; // Total Slice PRB a instance can provide 
    std::unordered_map<std::string, int> Slice_capacity ; // Slice PRB a instance can provide
    std::unordered_map<std::string, int> Slice_load ;      // Slice PRB a instance have been used
    std::unordered_map<std::string, float> Slice_utilization ;  // Slice_load / Slice_prb_avail 
 
    float throughput;
    Cell(){}
    Cell(std::string name, int cell_prb_avail) {
        Name = name;
 
        Cell_prb_avail = cell_prb_avail;
 
    }
};

//----------------------------------------------------------------------
// Ken declares function
//----------------------------------------------------------------------
void get_dummy_data(vector <unordered_map<string , string>> &ue_map_group, vector <unordered_map<string , string>> &cell_map_group);
void get_influxdata(string data ,vector< unordered_map<string, string> > &data_group);
void query_influxdb( std::vector<UE> &UE_Group, std::vector<Cell> &Cell_Group, std::vector<std::string> &Slice_list);
void slice_allocation(std::vector<UE> &ue_list, std::vector<Cell> &cell_list, std::vector<std::string> &slice_list);
void handover_ue(int &ue_k_key,UE &ue_k, std::vector<UE> &ue_list, std::vector<Cell> &cell_list, std::vector<std::string> &slice_list, Updata_scenario update);
//void update_load(std::vector<UE> &ue_list, std::vector<Cell> &cell_list, std::vector<std::string> &slice_list);
void update_load_b(UE &ue_k, Cell &new_bs, std::vector<std::string> &slice_list, std::unordered_map <std::string, int> &slice_provide_map, Updata_scenario update);
void print_all_group(std::vector<UE> UE_Group, std::vector<Cell> Cell_Group, std::vector<std::string> Slice_list);
void uniform_random_slice_prb(int lower, int upper , unordered_map < string , int > &ue_slice_prb);
vector<string> slice_parser(string ue_slice);


 

void policy_callback( Message& mbuf, int mtype, int subid, int len, Msg_component payload,  void* data ) {

  int response_to = 0;	 // max timeout wating for a response
  int rmtype;		// received message type

  string arg ((const char*)payload.get(), len); // RMR payload might not have a nil terminanted char

  cout << "[INFO] Policy Callback got a message, type=" << mtype << ", length="<< len << "\n";
  cout << "[INFO] Payload is " << arg << endl;

  PolicyHandler handler;
  Reader reader;
  StringStream ss(arg.c_str());
  reader.Parse(ss,handler);

  //Set the threshold value
  if (handler.found_threshold) {
    cout << "[INFO] Setting RSRP Threshold to A1-P value: " << handler.threshold << endl;
    rsrp_threshold = handler.threshold;
  }

  mbuf.Send_response( 101, -1, 5, (unsigned char *) "OK1\n" );	// validate that we can use the same buffer for 2 rts calls
  mbuf.Send_response( 101, -1, 5, (unsigned char *) "OK2\n" );
}

// callback to handle handover reply (json http response)
size_t handoff_reply_callback( const char *in, size_t size, size_t num, string *out ) {
  const size_t totalBytes( size * num );
  out->append( in, totalBytes );
  return totalBytes;
}

// sends a handover message through REST
void send_rest_control_request( string msg ) {
  CURL *curl = curl_easy_init();
  curl_easy_setopt( curl, CURLOPT_URL, ts_control_ep.c_str() );
  curl_easy_setopt( curl, CURLOPT_TIMEOUT, 10 );
  curl_easy_setopt( curl, CURLOPT_POST, 1L );
  // curl_easy_setopt(curl, CURLOPT_VERBOSE, 1L);

  // response information
  long httpCode( 0 );
  unique_ptr<string> httpData( new string() );

  curl_easy_setopt( curl, CURLOPT_WRITEFUNCTION, handoff_reply_callback );
  curl_easy_setopt( curl, CURLOPT_WRITEDATA, httpData.get());
  curl_easy_setopt( curl, CURLOPT_POSTFIELDS, msg.c_str() );

  struct curl_slist *headers = NULL;  // needs to free this after easy perform
  headers = curl_slist_append( headers, "Accept: application/json" );
  headers = curl_slist_append( headers, "Content-Type: application/json" );
  curl_easy_setopt( curl, CURLOPT_HTTPHEADER, headers );

  cout << "[INFO] Sending a HandOff CONTROL message to \"" << ts_control_ep << "\"\n";
  cout << "[INFO] HandOff request is " << msg << endl;

  // sending request
  CURLcode res = curl_easy_perform( curl );
  if( res != CURLE_OK ) {
    cout << "[ERROR] curl_easy_perform() failed: " << curl_easy_strerror( res ) << endl;

  } else {

    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);
    if( httpCode == 200 ) {
      // ============== DO SOMETHING USEFUL HERE ===============
      // Currently, we only print out the HandOff reply
      rapidjson::Document document;
      document.Parse( httpData.get()->c_str() );
      rapidjson::StringBuffer s;
	    rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(s);
      document.Accept( writer );
      cout << "[INFO] HandOff reply is " << s.GetString() << endl;


    } else if ( httpCode == 404 ) {
      cout << "[ERROR] HTTP 404 Not Found: " << ts_control_ep << endl;
    } else {
      cout << "[ERROR] Unexpected HTTP code " << httpCode << " from " << ts_control_ep << \
              "\n[ERROR] HTTP payload is " << httpData.get()->c_str() << endl;
    }

  }

  curl_slist_free_all( headers );
  curl_easy_cleanup( curl );
}

// sends a handover message to RC xApp through gRPC
void send_grpc_control_request() {
  grpc::ClientContext context;
  api::RicControlGrpcReq *request = api::RicControlGrpcReq().New();
  api::RicControlGrpcRsp response;

  api::RICE2APHeader *apHeader = api::RICE2APHeader().New();
  api::RICControlHeader *ctrlHeader = api::RICControlHeader().New();
  api::RICControlMessage *ctrlMsg = api::RICControlMessage().New();

  request->set_e2nodeid("10110101110001100111011110001");
  request->set_plmnid("373437");
  request->set_ranname("gnb_734_733_b5c67788");
cout << "[Ken_Debug] Set Parameters \n " << endl ;
cout << "[Ken_Debug] Set UEID \n  "      << endl ;
cout << "[Ken_Debug] Set targetcell \n " << endl ;

ctrlHeader->set_ueid("Waiting passenger 7");
ctrlMsg->set_targetcellid("c2B13");

  request->set_allocated_rice2apheaderdata(apHeader);
  request->set_allocated_riccontrolheaderdata(ctrlHeader);
  request->set_allocated_riccontrolmessagedata(ctrlMsg);
  request->set_riccontrolackreqval(api::RIC_CONTROL_ACK_UNKWON);  // not yet used in api.proto

  grpc::Status status = rc_stub->SendRICControlReqServiceGrpc(&context, *request, &response);
  cout << "[Ken_Debug] Send RIC Control Request" << endl ; 
  if(status.ok()) {
    /*
      TODO check if this is related to RICControlAckEnum
      if yes, then ACK value should be 2 (RIC_CONTROL_ACK)
      api.proto assumes that 0 is an ACK
    */ 
    cout << "[Ken_Debug] status OK" << endl ;
    if(response.rspcode() == 0) {
      cout << "[Ken_Debug] Send RIC Control Successfully " << endl ;
      cout << "[INFO] Control Request succeeded with code=0, description=" << response.description() << endl;
    } else {
      cout << "[Ken_Debug] Send RIC Control failed" << endl ;
      cout << "[ERROR] Control Request failed with code=" << response.rspcode()
           << ", description=" << response.description() << endl;
    }

  } else {
    cout << "[Ken_Debug] Send RIC Control failed error_code = ?? " << endl ;
    cout << "[ERROR] failed to send a RIC Control Request message to RC xApp, error_code="
         << status.error_code() << ", error_msg=" << status.error_message() << endl;
  }

  // FIXME needs to check about memory likeage
}

 
//----------------------------------------------------------------------
// Ken defines function , print_all_group
// output the outcome of data structure of UE and Cell 
// to the file , "outcome_cell.csv" and "outcome_ue.csv"
// after slice allocation
//----------------------------------------------------------------------
void print_all_group(std::vector<UE> UE_Group, std::vector<Cell> Cell_Group, std::vector<std::string> Slice_list){


    float slice_utilization = 0;
    string outcome = "outcome_cell.csv";

    ifstream fin(outcome.c_str(), std::ios::in);
    stringstream buffer;
    ofstream fout(outcome.c_str(), std::ios::out);


    buffer << "Cell_Name" <<  "," << "Total_PRB" << "," ;
    buffer << "slice_capacity_embb" << "," << "slice_capacity_urllc" << "," << "slice_capacity_mmtc" << "," ;
    buffer << "slice_load_embb" << "," << "slice_load_urllc" << "," << "slice_load_mmtc" << "," ;
    buffer << "slice_utilization_embb" << "," << "slice_utilization_urllc" << "," << "slice_utilization_mmtc" << "," ;
    buffer << endl;


    for(int n=0;n<Cell_Group.size();n++){
        if(fin.good()){
            buffer << Cell_Group[n].Name << "," << Cell_Group[n].Cell_prb_avail << ",";
        }  

        for (int s = 0 ; s < Slice_list.size(); s++){
            if(fin.good()){
                buffer << Cell_Group[n].Slice_capacity[Slice_list[s]] << "," ;
            }  
        }
       
        for (int s = 0 ; s < Slice_list.size(); s++){
            if(fin.good()){
                buffer << Cell_Group[n].Slice_load[Slice_list[s]] << "," ;
            }  
        }

        for (int s = 0 ; s < Slice_list.size(); s++){
            if(fin.good()){
                buffer << Cell_Group[n].Slice_utilization[Slice_list[s]]  << "," ;
            }  
        }

        if(fin.good()){
            buffer << endl;
        } 
    }


    fout << buffer.rdbuf();

    outcome = "outcome_ue.csv";
    ifstream fin_ue(outcome.c_str(), std::ios::in);
    stringstream buffer_ue;
    ofstream fout_ue(outcome.c_str(), std::ios::out);



    buffer_ue << "UE_Name" <<  "," << "Serving_Cell_Name" << "," ;
    buffer_ue << "slice_prb_used_embb" << "," << "slice_prb_used_urllc" << "," << "slice_prb_used_mmtc" << "," ; 
    buffer_ue << endl;

    for(int k=0;k<UE_Group.size();k++){
        if(fin_ue.good()){
            buffer_ue << UE_Group[k].Name  << "," <<  UE_Group[k].Serv_cell <<  ",";
        } 
        for(int i = 0 ; i < Slice_list.size() ; i++){
            if(fin_ue.good()){
                if(UE_Group[k].Slice_prb_req.find(Slice_list[i]) == UE_Group[k].Slice_prb_req.end()){
                    buffer_ue << "," ;
                }else{
                    buffer_ue << UE_Group[k].Slice_prb_req[Slice_list[i]] << "," ;
                }
            }
        }
        if(fin_ue.good()){
            buffer_ue << endl;
        }
    }    

    
    buffer_ue << endl;
    fout_ue << buffer_ue.rdbuf();
}
//----------------------------------------------------------------------
// Ken defines function , uniform_random_slice_prb
// responsible for uniformly random the slice PRB to the slice the UE is using
// here we have three slices ,  embb , mmtc , urllc
// note: the sum of all the slice PRB will be total PRB
//----------------------------------------------------------------------
void uniform_random_slice_prb(int lower, int upper , unordered_map < string , int > &ue_slice_prb){
    // get the rand num
    std::default_random_engine rand_num{static_cast<long unsigned int>(std::chrono::high_resolution_clock::now().time_since_epoch().count())};
    uniform_int_distribution<> dist(lower,upper); 
    vector<int> random_num ;  
    for (int i = 0; i < ue_slice_prb.size(); ++i) {
        random_num.push_back(dist(rand_num)) ; // pass the generator to the distribution.
    }

    
    int random_num_sum = 0;
    for(int i =0 ; i < random_num.size();i++){
        //cout << "[Debug] random is " << random_num[i] << "\n";
        random_num_sum += random_num[i] ;
    }
    float random_num_scaler =   (float)upper/random_num_sum ;  // scale to the slice prb ue request
    int temp_min = 999; // use INT_MAX shall be better
    random_num_sum=0; 

    // define the mininum slice prb randomly generated 
    // after distribute the slice prb
    // the sum of distributed slice prb == slice prb ue request - 1
    // so distribute one prb to the mininum slice (may have better way)
    string min_prb_slice = ""; 
    int random_num_index = -1 ;


    // distribute the slice prb to the ue request
    for(const auto&s : ue_slice_prb){
        string ueslice = s.first;
        auto iter = Scenario_table.find(ueslice);
        random_num_index++;
        if(iter != Scenario_table.end() ){
            switch (iter->second)
            {
            case Scenario::embb:
                ue_slice_prb["embb"] = random_num[random_num_index]*random_num_scaler ;
                break;
            case Scenario::mmtc:
                ue_slice_prb["mmtc"] = random_num[random_num_index]*random_num_scaler ;
                break;
            case Scenario::urllc:
                ue_slice_prb["urllc"] = random_num[random_num_index]*random_num_scaler ;
                break;            
            default:
                break;
            }
        }else{
            ue_slice_prb["other_slice"] = random_num[random_num_index]*random_num_scaler ;
        }

        // find the slice have the mininum prb
        // distribute one prb to it
        random_num_sum += s.second ;
        if(temp_min > s.second){
            temp_min = s.second;
            min_prb_slice = s.first;
        }
    }
    ue_slice_prb[min_prb_slice] += upper-random_num_sum; // one prb
}

//----------------------------------------------------------------------
// Ken defines function , slice_parser
// before read the dummy data, we need to assign UE which kind of slices will use  as string 
// and parse it into how many slice the UE use and which kind of slice the UE use
//----------------------------------------------------------------------
vector<string> slice_parser(string ue_slice){

    // the string we read from the file 
    // will be like 
    // slice1_slice2_slice3 ...
    // we need to the where   icon "_" is to divide the string
    int icon_position = 0;
    vector<string> ue_slices;         
    string ue_slice_temp;               
    for(int s = 0 ; s < ue_slice.length()+1 ; s++){
        if('_' == ue_slice[s] or ue_slice[s] == '\0'){
            for(int ss = icon_position ; ss<  s; ss++){
                ue_slice_temp +=  ue_slice[ss];
            }
            ue_slices.push_back(ue_slice_temp);
            ue_slice_temp = "";
            icon_position = s+1; 
        }
    }
    return ue_slices;
}

//----------------------------------------------------------------------
// Ken defines function , get_dummy_data
// read the data store in "valid.csv" and "MeasReport_cell.csv"
// File_length is fixed
// TODO : how to read any csv without knowing its row length
//----------------------------------------------------------------------
void get_dummy_data(vector <unordered_map<string , string>> &ue_map_group, vector <unordered_map<string , string>> &cell_map_group){
    ifstream ueFile;
    ueFile.open("valid.csv");
    int File_length = 22;


    string line;
    int n = 1;//desired row number
    int a = 0;//counter
    int a_map_cnt = 0;
    string header ; 
    string mesg ; 
    
    
    unordered_map<string , string> data_map;
    string header_string_temp;
    vector <string> header_string ;
    string mesg_string_temp;
    vector <string> mesg_string ;

    int icon_position = 0;
    while (getline(ueFile, line)) {
        a++;
        if(a == File_length){
            break ;
        }
        if(a == 1){
           header   =  line; 
            for(int s = 0 ; s < header.length()+1 ; s++){
                if(',' == header[s] or header[s] == '\0'){
                    for(int ss = icon_position ; ss<  s ; ss++){
                        header_string_temp +=  header[ss];
                    }
                    header_string.push_back(header_string_temp);
                    header_string_temp = "";
                    icon_position = s+1; 
                }
            }
        }else{
            mesg = line ; 
            //cout << mesg << "\n";
            mesg_string_temp = "" ; 
            mesg_string.clear();
            icon_position = 0 ;
            for(int s = 0 ; s < mesg.length()+1 ; s++){
                if(',' == mesg[s] or mesg[s] == '\0'){
                    for(int ss = icon_position ; ss<  s ; ss++){
                        mesg_string_temp +=  mesg[ss];
                    }
                    mesg_string.push_back(mesg_string_temp);
                    mesg_string_temp = "";
                    icon_position = s+1; 
                }
            }
            for(int h = 0 ; h < header_string.size();h++){
                data_map[header_string[h]] =  mesg_string[h];
                //cout << "h is :" << h << "data is : "<< header_string[h] <<  " : " <<  data_map[header_string[h]] << "\n";
                //cout << header_string[a]<< "\n";
            }
           // cout <<  "outside  h is : " << h << "\n" ;
            ue_map_group.push_back(data_map);
            data_map.clear();
        }
    }
    ueFile.close();


    ifstream cellFile;
    cellFile.open("MeasReport_cell.csv");
    File_length = 41;

    line = "";
    n = 1;//desired row number
    a = 0;//counter
    a_map_cnt = 0;
    header =""; 
    mesg=""; 
    
     
    data_map.clear();
 
    header_string_temp="";
    header_string.clear();

    mesg_string_temp="";
    mesg_string.clear();

    icon_position = 0;

    while (getline(cellFile, line)) {
      a++;
        if(a == File_length){
            break ;
        }
        if(a == 1){
           header   =  line; 
          
            for(int s = 0 ; s < header.length()+1 ; s++){
                if(',' == header[s] or header[s] == '\0'){
                    for(int ss = icon_position ; ss<  s ; ss++){
                        header_string_temp +=  header[ss];
                    }
                    header_string.push_back(header_string_temp);
                    header_string_temp = "";
                    icon_position = s+1; 
                }
            }
        }else{
            mesg = line ; 
            //cout << mesg << "\n";
            mesg_string_temp = "" ; 
            mesg_string.clear();
            icon_position = 0 ;
            for(int s = 0 ; s < mesg.length()+1 ; s++){
                if(',' == mesg[s] or mesg[s] == '\0'){
                    for(int ss = icon_position ; ss<  s ; ss++){ 
                        mesg_string_temp +=  mesg[ss];  
                    }
                    mesg_string.push_back(mesg_string_temp);
                    mesg_string_temp = "";
                    icon_position = s+1; 
                }
            }
            for(int h = 0 ; h < header_string.size();h++){
                data_map[header_string[h]] =  mesg_string[h];
                //cout << "h is :" << h << "data is : "<< header_string[h] <<  " : " <<  data_map[header_string[h]] << "\n";
                //cout << header_string[a]<< "\n";
            }
           // cout <<  "outside  h is : " << h << "\n" ;
            cell_map_group.push_back(data_map);
            data_map.clear();
        }
    }
}
 
//----------------------------------------------------------------------
// Ken defines function , get_influxdata
// this function will not be used  since we didn't use run time data
// just defined here , it's not used currently
// TODO : get real time data from influxDB
//----------------------------------------------------------------------	
void get_influxdata(string data ,vector< unordered_map<string, string> > &data_group){

    unordered_map<string , string> data_map;
    string data_string_temp;
    vector<string> data_string;
    int icon_position = 0;

     
     for(int s = 0 ; s < data.length()+1 ; s++){
        if(',' == data[s] or data[s] == '\0'){
            for(int ss = icon_position ; ss<  s ; ss++){
                data_string_temp +=  data[ss];
            }
            data_string.push_back(data_string_temp);
            data_string_temp = "";
            icon_position = s+1; 
        }
    }
    icon_position = 0;
    //cout << ue_data_string[2];
    for(int i = 0 ; i < data_string.size() ; i++){
        for(int j = 0 ; j < data_string[i].length() ; j++){
            if( data_string[i][j] == '=' ){
                string key ;
                string value;
                icon_position = j ;
                for(int k = 0; k < icon_position ; k++)
                    key += data_string[i][k];

                for(int v = icon_position + 1 ; v < data_string[i].length() ; v++)
                    value += data_string[i][v];
                
                
                data_map[key] = value;
            }
        }
    }
	data_group.push_back(data_map);
}

 
//----------------------------------------------------------------------
// Ken defines function , query_influxdb
// current, this function will use dummy data set
// get the data , and store it to the struct defined in UE & Cell
// TODO : get real time data from influxDB
//----------------------------------------------------------------------	
void query_influxdb( std::vector<UE> &UE_Group, std::vector<Cell> &Cell_Group, std::vector<std::string> &Slice_list){
        

    // Initialize cell support slice
    Slice_list.push_back("embb");
    Slice_list.push_back("urllc");
    Slice_list.push_back("mmtc");
    int Slice_size = Slice_list.size();    



    // Get dummy data , and store it
    vector< unordered_map<string, string> > ue_group ;
    vector< unordered_map<string, string> > cell_group ;

    get_dummy_data(ue_group, cell_group);
    
  

    // debug retrieve data
    //string test_string  = "ue-id=Pedenstrian-1,nrCellIdentity=N87,prb_usage=55,nbCellIdentity_0=C13,nbCellIdentity_1=C12,nbCellIdentity_2=B12,nbCellIdentity_3=A11,nbCellIdentity_4=X22";
    //test_string_vector.push_back(test_string);

    // Print UE Information 
    for(int i=0;i<ue_group.size();i++){
        //get_influxdata(ue_data[i].getFields() , ue_group);
        //get_influxdata(test_string_vector[i] , ue_group);

        // avoid stoi error
        if(ue_group[i]["prb_usage"] == ""){
            ue_group[i]["prb_usage"] = "0";
        }
        // debug retrieve data
        //
        //      cout << "[Ken_Debug] : " << "ue-id : "     << ue_group[i]["ue-id"] << "\n";
        //      cout << "[Ken_Debug] : " << "Serv_Cell : " << ue_group[i]["nrCellIdentity"]<< "\n";
        //      cout << "[Ken_Debug] : " << "PRB_using : " << stoi(ue_group[i]["prb_usage"]) << "\n";
        //      cout << "[Ken_Debug] : " << "NR_Cell 0 : " << ue_group[i]["nbCellIdentity_0"]<< "\n";
        //      cout << "[Ken_Debug] : " << "NR_Cell 1 : " << ue_group[i]["nbCellIdentity_1"]<< "\n";
        //      cout << "[Ken_Debug] : " << "NR_Cell 2 : " << ue_group[i]["nbCellIdentity_2"]<< "\n";
        //      cout << "[Ken_Debug] : " << "NR_Cell 3 : " << ue_group[i]["nbCellIdentity_3"]<< "\n";
        //      cout << "[Ken_Debug] : " << "NR_Cell 4 : " << ue_group[i]["nbCellIdentity_4"]<< "\n";
        //      cout << "[Ken_Debug] : " << "Slices : " << ue_group[i]["Slices"]<< "\n";
        //
        mdclog_write(MDCLOG_INFO, "UE Info:  ue-id=%s , Serv_Cell=%s, PRB_using= %d, NR_Cell_0=%s, NR_Cell_1=%s, NR_Cell_2=%s, NR_Cell_3=%s, NR_Cell_4=%s, Slices=%s", 
        ue_group[i]["ue-id"].c_str(), ue_group[i]["nrCellIdentity"].c_str(), stoi(ue_group[i]["prb_usage"]), ue_group[i]["nbCellIdentity_0"].c_str(), ue_group[i]["nbCellIdentity_1"].c_str(),
        ue_group[i]["nbCellIdentity_2"].c_str(), ue_group[i]["nbCellIdentity_3"].c_str(), ue_group[i]["nbCellIdentity_4"].c_str(), ue_group[i]["Slices"].c_str());
    }

    // debug retrieve data
    //test_string = "nrCellIdentity=N87,availPrbDl=122";
    for(int i=0;i<cell_group.size();i++){
        //get_influxdata(cell_data[i].getFields() , cell_group);
        //get_influxdata(test_string_vector[i] , cell_group);


        // avoid stoi error
        if(cell_group[i]["availPrbDl"] == ""){
            cell_group[i]["availPrbDl"] = "0";
        }
        // debug retrieve data
        //cout << "[Ken_Debug] : " << "Cell Name : " << cell_group[i]["nrCellIdentity"] << "\n";
        //cout << "[Ken_Debug] : " << "Available PRB DL : " << stoi(cell_group[i]["availPrbDl"]) << "\n";


        mdclog_write(MDCLOG_INFO,"Cell Info:  Cell_Name=%s , Available_PRB_DL=%d", cell_group[i]["nrCellIdentity"].c_str(), stoi(cell_group[i]["availPrbDl"]) );
    }


    // Initialize cell
    for(int i = 0 ; i< cell_group.size() ; i++)
        Cell_Group.push_back(Cell(cell_group[i]["nrCellIdentity"] , stoi(cell_group[i]["availPrbDl"])));



    for(int n = 0 ; n < Cell_Group.size(); n++){
    
        Cell_Group[n].Slice_prb_avail["embb"] = 0 ;
        Cell_Group[n].Slice_prb_avail["mmtc"] = 0 ;
        Cell_Group[n].Slice_prb_avail["urllc"] = 0 ;
        Cell_Group[n].Slice_capacity["embb"] = 0 ;
        Cell_Group[n].Slice_capacity["mmtc"] = 0 ;
        Cell_Group[n].Slice_capacity["urllc"] = 0 ;
        Cell_Group[n].Slice_load["embb"] = 0 ;
        Cell_Group[n].Slice_load["mmtc"] = 0 ;
        Cell_Group[n].Slice_load["urllc"] = 0 ;
        Cell_Group[n].Slice_utilization["embb"] = 0 ;
        Cell_Group[n].Slice_utilization["mmtc"] = 0 ;
        Cell_Group[n].Slice_utilization["urllc"] = 0 ;         
    }



    mdclog_write(MDCLOG_INFO,"Start Init Cell Slice PRB");


    // store Cell from  dummy data got before   
    // TODO: cell may not support every slice but RIC Test now support all in default 
    for(int n = 0 ; n < Cell_Group.size(); n++){
        for (int s = 0 ; s < Slice_list.size(); s++){

            
            Cell_Group[n].Slice_prb_avail[Slice_list[s]] = Cell_Group[n].Cell_prb_avail/Slice_size;
            //std::cout  << Cell_Group[n].Slice_prb_avail[Slice_list[s]] << "\n"; 


            mdclog_write(MDCLOG_INFO,"Cell_Name=%s , Slice=%s , Slice_PRB=%d", Cell_Group[n].Name.c_str(), Slice_list[s].c_str(), Cell_Group[n].Slice_prb_avail[Slice_list[s]] );
 
            Cell_Group[n].Slice_capacity[Slice_list[s]] =  Cell_Group[n].Cell_prb_avail/Slice_size;//bn,s Remaining slice prb can provide to ue
            Cell_Group[n].Slice_load[Slice_list[s]] = 0;  // current slice prb provide users 
            Cell_Group[n].Slice_utilization[Slice_list[s]] = 0; // provide / total
        }
    }

    // Initialize ue
    std::vector<std::string> nrcell_temp;  // coverage


    // store Cell from  dummy data got before   
    for(int i = 0 ; i< ue_group.size() ; i++){
        nrcell_temp.push_back(ue_group[i]["nbCellIdentity_0"]);
        nrcell_temp.push_back(ue_group[i]["nbCellIdentity_1"]);
        nrcell_temp.push_back(ue_group[i]["nbCellIdentity_2"]);
        nrcell_temp.push_back(ue_group[i]["nbCellIdentity_3"]);
        nrcell_temp.push_back(ue_group[i]["nbCellIdentity_4"]);

        UE_Group.push_back(UE(ue_group[i]["ue-id"] , ue_group[i]["nrCellIdentity"],nrcell_temp)); //name, x, y, serv_cell, nr_cell
       

        //Init slice prb 
        vector <string> ueslice = slice_parser(ue_group[i]["Slices"]) ;
        for(int j = 0  ; j < ueslice.size();j++){
            //cout << ueslice[j] << "\n";

            UE_Group[i].Slice_prb_req[ueslice[j]] = 0 ; 
 
        }
        nrcell_temp.clear();
        //Initialize Random PRB
        uniform_random_slice_prb(0,stoi(ue_group[i]["prb_usage"]), UE_Group[i].Slice_prb_req);
    }


    mdclog_write(MDCLOG_INFO,"Start Init UE Slice PRB");


    //Print UE Information 
    
    for(int i=0;i<ue_group.size();i++){

        mdclog_write(MDCLOG_INFO,"ue-id=%s , PRB_using=%d", ue_group[i]["ue-id"].c_str(), stoi(ue_group[i]["prb_usage"]) );
    }

    for(int i=0;i<UE_Group.size();i++){

        for(const auto&s : UE_Group[i].Slice_prb_req){
            //cout << "slice: " << s.first << ", " << " prb is : " << s.second << "\n";
            mdclog_write(MDCLOG_INFO,"Slice=%s , Slice_PRB=%d", s.first.c_str() , s.second );
         
        }
        
    }


}
       
//----------------------------------------------------------------------
// Ken defines function , slice_allocation
// code flow chart is depicted here https://hackmd.io/NvYiLkJ9SvONbUWnis2RbQ?view#Slice-Allocation
// It only accept the three argument below
// 1. std::vector<UE> ue_list, 
// 2. std::vector<Cell> cell_list, 
// 3. std::vector<std::string> slice_list

// struct UE and struct Cell has already defined in the ts_xapp.cpp

// For now, slice list only accept three slice , 
// respectively , embb , mmtc , urllc

// declear the vector variable of UE and Cell , 
// and then read your dummy data to use it!
//----------------------------------------------------------------------	

void slice_allocation(std::vector<UE> &ue_list, std::vector<Cell> &cell_list, std::vector<std::string> &slice_list){
    // do slice allocation

    Updata_scenario updata_scenario;

    // iterate all the ue
    for(int k = 0 ; k < ue_list.size() ; k++){

        

        mdclog_write(MDCLOG_INFO,"Start Slice PRB Allocation Algorithm" );

        mdclog_write(MDCLOG_DEBUG,"ue-id %s", ue_list[k].Name.c_str() );

      
        //std::cout << "[Debug] operating ue : " << ue_list[k].Name << "\n";

        // distribute ue to a cell
        handover_ue(k, ue_list[k], ue_list, cell_list, slice_list, updata_scenario);    
      


        // update neighbor cells slice utilization
        ue_list[k].NR_Slice_utilization.resize(5);

        for(int ue_nr=0 ; ue_nr < ue_list[k].NR_cell.size() ; ue_nr++){
                
            for(int c = 0 ; c < cell_list.size() ; c++){
                if(cell_list[c].Name == ue_list[k].Serv_cell){
                    ue_list[k].serv_slice_utilization["urllc"] = cell_list[c].Slice_utilization["urllc"] ; 
                    ue_list[k].serv_slice_utilization["embb"] = cell_list[c].Slice_utilization["embb"] ; 
                    ue_list[k].serv_slice_utilization["mmtc"]  = cell_list[c].Slice_utilization["mmtc"] ; 
                }
                if(cell_list[c].Name == ue_list[k].NR_cell[ue_nr]){

                    ue_list[k].NR_Slice_utilization[ue_nr]["urllc"] = cell_list[c].Slice_utilization["urllc"] ;
                    ue_list[k].NR_Slice_utilization[ue_nr]["embb"] = cell_list[c].Slice_utilization["embb"] ;
                    ue_list[k].NR_Slice_utilization[ue_nr]["mmtc"] = cell_list[c].Slice_utilization["mmtc"] ;                    
            
                    
                }
            }
        }

        mdclog_write(MDCLOG_INFO,"Complete Slice PRB Allocation Algorithm" );

        mdclog_write(MDCLOG_INFO,"Update the InfluxDB..." );


     
    }
}

//----------------------------------------------------------------------
// Ken defines function , handover_ue
// distribute ue to a cell 
// according to three condition
// 1. if serving cell can provide slices prb to ue ?
// 2. else if does neighbor cells exist ?
// 3. else handover another ue occupy the most resources to other cell
//----------------------------------------------------------------------	
void handover_ue(int &ue_k_key,UE &ue_k, std::vector<UE> &ue_list, std::vector<Cell> &cell_list, std::vector<std::string> &slice_list, Updata_scenario update){
    // check any requested slices by UE k is full
    bool slice_full  = false;
  
    int new_bs_key; // index of the cell will severs ue after going through the function
    int ue_k_bs_key; // index of the  serving cell
    vector<string> ue_k_slice_name ;
    // get index of the  serving cell
    for(int n = 0; n<cell_list.size();n++){
        if(cell_list[n].Name==ue_k.Serv_cell){
            ue_k_bs_key =n ;    
        }
    } 

    // for a cell , each cell have three slice , and each slice is evenly distributed total prb
    // just slice embb * 3 
    mdclog_write(MDCLOG_DEBUG,"Current Serving Cell is %s  Cell Total PRB is %d", ue_k.Serv_cell.c_str() , (cell_list[ue_k_bs_key].Slice_capacity["embb"])*3);

    //std::cout << "[Debug] Serv cell is :" << ue_k.Serv_cell << "\n" ; 

 
  	//std::cout << "[Debug] Serv cell prb is  :" << cell_list[ue_k_bs_key].Slice_capacity["embb"] << "\n"; 
    


  // check if serving cell can provide all the ue requested slice prb
	for(const auto&s : ue_k.Slice_prb_req){
        ue_k_slice_name.push_back(s.first);
        //std::cout << "[Debug] ue_k slice prb req :"<< s.first << ","<< s.second << "\n" ; 

        mdclog_write(MDCLOG_DEBUG,"slice prb req is %s %d", s.first.c_str(), s.second );


        mdclog_write(MDCLOG_DEBUG,"serving cell slice prb is %d", cell_list[ue_k_bs_key].Slice_capacity[s.first] );

        if(cell_list[ue_k_bs_key].Slice_capacity[s.first]   < s.second){
            
            slice_full = true ; 

            break;
        }
        
    }
 
    // 1. serving cell can provide slices prb to ue
    if(slice_full==false){
       
        new_bs_key = ue_k_bs_key;
        update = stay;

        mdclog_write(MDCLOG_DEBUG,"slice is not full, keep serve the ue");

    }else{

        mdclog_write(MDCLOG_DEBUG,"ue-id %s   serving cell %s is full, unable to serve the resources needed, start to do handover procedure", ue_k.Name.c_str(), ue_k.Serv_cell.c_str());

        //std::cout << "[Debug] Serv cell  :" << ue_k.Serv_cell << " is full \n"; 
    }

    // 2. serving cell can't provide slices prb to ue
    //    if does neighbor cells exist ?
    if(slice_full==true){

        // check if  neighbor cells exist
        // 3. handover another ue occupy the most resources to other cell
        if(ue_k.NR_cell.size() == 0) {

            mdclog_write(MDCLOG_DEBUG,"ue-id %s   doesn't have neighbor cell, unable to handover it another cell", ue_k.Name.c_str());

            mdclog_write(MDCLOG_DEBUG,"start to find an another UE occupying the most number of resources, preparing to handover it");

            //std::cout << "[Debug] enter No NR_cell \n"; 
            //std::cout << "[Debug] enter handover procedure \n"; 
            // Get the list of UE connecting to the BS n such that NR.cell.size==0
            // find UE k2 occuying the most number of slices UE k request
            int occuying_slice_num = 0;
            int occuying_slice_prb = 0;
            int temp_total_slice_prb = 0;
            int ue_k2_key = 0;
        
            for(int k =0;k<ue_list.size();k++){
                if(k!=ue_k_key){

                    if(ue_k.Serv_cell == ue_list[k].Serv_cell && ue_list[k].NR_cell.size()>0){
                  
                        if(ue_list[k].Slice_prb_req.size() > occuying_slice_num){
                            for(const auto&s :  ue_list[k].Slice_prb_req){
                                temp_total_slice_prb += s.second ;
                        
                            }

                            if( temp_total_slice_prb  > occuying_slice_prb) {
                            
                                ue_k2_key  = k ;
                                occuying_slice_prb =  temp_total_slice_prb ;
                            }
                        }    
                        occuying_slice_num =  ue_list[k].Slice_prb_req.size();               
                    }
                
                }

                
            }

            // find the emptiest cell that 
            // is in UE_k2 exclude BS n NR_cell.size() == 0
            int next_bs_index = 0 ;
            int slice_prb_avail_temp = 0;
            int slicecapacity = 0 ;

            for(int n=0;n<ue_list[ue_k2_key].NR_cell.size();n++){
                for(int nn=0;nn<cell_list.size();nn++){
                    for(int s = 0 ; s < slice_list.size() ; s++){
                        slicecapacity = cell_list[nn].Slice_capacity[slice_list[s]] ; 
                    }

                    if(ue_list[ue_k2_key].NR_cell[n] == cell_list[nn].Name){
                        if(slicecapacity > slice_prb_avail_temp){
                            next_bs_index = nn;
                            slice_prb_avail_temp = slicecapacity;
                        }   
                        
                    }
                }
            }


            mdclog_write(MDCLOG_DEBUG,"find the UE %s occupying the most number of resources", ue_list[ue_k2_key].Name.c_str());
            mdclog_write(MDCLOG_DEBUG,"find the Cell %s owing the most number of resources another UE need", cell_list[next_bs_index].Name.c_str());
            mdclog_write(MDCLOG_DEBUG,"start to handover both UE...");

            //std::cout << "[Debug] handover another ue  : " << ue_list[ue_k2_key].Name << "\n";
            //std::cout << "[Debug] To BS  : " << cell_list[next_bs_index].Name << "\n";        
            
            // Check if the slice next bs ue_k and ue_k2 use is greater than current ue_k2 use
            // and also check if all slices will be provided after handover  
            // define provide slice or not is using slice prb avail (total slice)

            // calculating bn,s >= fs,n + rs
            int next_provide_ue_k2_slices = 0;
            int next_provide_ue_k_slices= 0;
            int curr_provide_ue_k2_slices= 0;
    
            
            for(const auto& s: ue_k.Slice_prb_req){
                if(cell_list[ue_k_bs_key].Slice_utilization[s.first] != 1){
                    if(cell_list[ue_k_bs_key].Slice_capacity[s.first]  >=  ue_k.Slice_prb_req[s.first] + cell_list[ue_k_bs_key].Slice_load[s.first])
                        next_provide_ue_k_slices++;
                }    
            }    
            
            int next_p_k2;
            int p_k2;
            bool no_handover ;
            for(const auto& s: ue_list[ue_k2_key].Slice_prb_req){
                if(cell_list[ue_k2_key].Slice_utilization[s.first] != 1){
                    if(cell_list[ue_k2_key].Slice_capacity[s.first]  >=  s.second + cell_list[ue_k2_key].Slice_load[s.first]){
                        curr_provide_ue_k2_slices++;
                        p_k2 = 1;
                    }
                    else{
                        p_k2 = 0;
                    }
                }
                else{
                    p_k2 = 0;
                }
                if(cell_list[next_bs_index].Slice_utilization[s.first] != 1){    
                    if(cell_list[next_bs_index].Slice_capacity[s.first] >= s.second + cell_list[next_bs_index].Slice_load[s.first]){
                        next_provide_ue_k2_slices++;
                        next_p_k2 = 1;
                    } 
                    else{
                        next_p_k2 = 0 ;
                    } 
                }  
                else {
                    next_p_k2 = 0;
                }

                if( (next_p_k2 - p_k2) == -1 ){
                    no_handover = true;
                }
            }
            // min(next_p_k2 - p_k2) > -1
            // 0 - 1 = -1 ! > -1 
            // means if there's one slice next_bs can't provide, the statement won't success

            if(next_provide_ue_k2_slices + next_provide_ue_k_slices >= curr_provide_ue_k2_slices && !no_handover){
                //std::cout << "[Debug] handover Confirm !!\n";
                
                // release  all the resources
                update = handover_ue_k2;
                int handover_bs_key = 0;
                for(int n = 0 ; n < cell_list.size(); n++){
                    if( cell_list[n].Name == ue_list[ue_k2_key].Serv_cell){
                        handover_bs_key = n ;
                        break;
                    }
                }

                mdclog_write(MDCLOG_DEBUG,"ue-id %s", ue_list[ue_k2_key].Name.c_str());

                mdclog_write(MDCLOG_DEBUG,"previous BS is %s", cell_list[handover_bs_key].Name.c_str());


                // release all the resources in its serving  Cell
                update_load_b(ue_list[ue_k2_key], cell_list[handover_bs_key], slice_list, ue_k.Slice_prb_req, update);


                // handover_request();
                // TODO: send handover request to a real cell

                // update all the value in the UE & Cell
                update = stay;
                for(const auto& s: ue_k.Slice_prb_req)
                    ue_k.Slice_prb_req[s.first] = s.second;

 
                // update all the value in the UE & Cell
                update_load_b(ue_k, cell_list[ue_k_bs_key], slice_list, ue_k.Slice_prb_req, update);

                // switch ue to neighbor cell
                update = switch_to_nr;
                for(const auto& s: ue_list[ue_k2_key].Slice_prb_req)
                    ue_k.Slice_prb_req[s.first] = s.second;

                // update all the value in  another UE & Cell
                update_load_b(ue_list[ue_k2_key], cell_list[next_bs_index], slice_list, ue_k.Slice_prb_req, update);

                mdclog_write(MDCLOG_DEBUG,"current BS is %s" , cell_list[next_bs_index].Name.c_str());
                mdclog_write(MDCLOG_DEBUG,"handover done");

            }
        }

        // 2. neighbor cells does exist 
        else{
    
            //std::cout << "[Debug] NR_cell != 0 \n"; 

            mdclog_write(MDCLOG_DEBUG,"ue-id %s   have neighbor cell,  handover it another cell", ue_k.Name.c_str());

            // define the neighbor cell and emptiest cell 
            unordered_map <int, Cell> next_bs_table ; 
            unordered_map <int, Cell> next_emp_bs_table ; 
          
            int curr_bs_capacity = 0;
            int next_bs_capacity = 0;
            
            // find all index of neighbor cells
            for(int n=0;n<ue_k.NR_cell.size();n++){
                for(int nn=0;nn<cell_list.size();nn++){  
                    if(ue_k.NR_cell[n] == cell_list[nn].Name ){   
                        auto next_bs_key = next_bs_table.find(nn);
                        if(next_bs_key ==next_bs_table.end()){
                            //std::cout << "[Debug] push new bs: " ;
                            //std::cout <<"nn" << nn << "next bs : " << next_bs_table[nn].Name <<"\n"; 
                            next_bs_table[nn]  = cell_list[nn] ; 
                            
                        }    
                        break;
                    }
                }
            }     
            

            int next_bs_key_temp = 0;
            
            Cell temp_cell = Cell();
           
            // see if this neighbor cells can provide slice prb to ue 
            for(const auto& next_bs_iter : next_bs_table){
                temp_cell = next_bs_iter.second ; 
                //std::cout   << "temp cell name " << temp_cell.Name << "\n";
                next_bs_key_temp = next_bs_iter.first ; 
                //std::cout   << "next_bs_key_temp is " <<  next_bs_key_temp <<"\n"; 
                slice_full =false ;
                for(int b = 0 ; b < ue_k_slice_name.size() ; b++){
                    //cout << "cell slice  " << ue_k_slice_name[b] << "  ,cell prb  " << temp_cell.Slice_capacity[ue_k_slice_name[b]] << "\n"; 
                    //cout << "ue slice  " << ue_k_slice_name[b] <<"  ,ue prb  " << ue_k.Slice_prb_req[ue_k_slice_name[b]] << "\n";
                    if(temp_cell.Slice_capacity[ue_k_slice_name[b]] < ue_k.Slice_prb_req[ue_k_slice_name[b]] ){
                        slice_full = true ; 
                     
                    }

                }
                if(slice_full != true){ 
                    next_emp_bs_table[next_bs_key_temp] = temp_cell ; 
                    //std::cout  << "next_bs name :" <<  next_emp_bs_table[next_bs_key_temp].Name <<"\n" ; 
                }
            }
 
             

            // find emptiest neighbor cell
            for(const auto& s: next_emp_bs_table){
                Cell cell_temp = s.second ;
                for(int b = 0 ; b < ue_k_slice_name.size() ; b++){
                    string slice_name = ue_k_slice_name[b] ; 
                    next_bs_capacity += cell_temp.Slice_capacity[slice_name] ;
                    //cout << "cell name is : " << s.second.Name << "\n";
                    //cout << "total capacity is : "<< next_bs_capacity  << "\n";
                }
                if(next_bs_capacity >= curr_bs_capacity){
                    curr_bs_capacity = next_bs_capacity ;  
                    new_bs_key = s.first ; 
                }
            }
             
              
              
            mdclog_write(MDCLOG_DEBUG,"find the Cell %s owing the most number of resources the UE need", cell_list[new_bs_key].Name.c_str());
            mdclog_write(MDCLOG_DEBUG,"start to handover the UE...");
            mdclog_write(MDCLOG_DEBUG,"ue-id %s", ue_k.Name.c_str());


            // switch ue to neighbor cell
            update = switch_to_nr;
            mdclog_write(MDCLOG_DEBUG,"previous BS is %s", ue_k.Serv_cell.c_str());
            // update all the value in the UE & Cell
            update_load_b(ue_k, cell_list[new_bs_key], slice_list, ue_k.Slice_prb_req, update);
            mdclog_write(MDCLOG_DEBUG,"current BS is %s" , ue_k.Serv_cell.c_str());
            mdclog_write(MDCLOG_DEBUG,"handover done");
        }
    }
    else{
        //std::cout  << "[Debug] ue_k new bs is : "<< cell_list[new_bs_key].Name << "\n";
        // update all the value in the UE & Cell
        update_load_b(ue_k, cell_list[new_bs_key], slice_list, ue_k.Slice_prb_req, update);
    }
    
}
//----------------------------------------------------------------------
// Ken defines function , update_load_b
// stay: update the value of serving cell
// switch_to_nr: switch ue to neighbor cell, update the value
// handover_ue_k2: release all the resources
//----------------------------------------------------------------------	
void update_load_b(UE &ue_k, Cell &new_bs, std::vector<std::string> &slice_list, std::unordered_map <std::string, int> &slice_provide_map, Updata_scenario update){

    // caculate the slice load & slice utililaztion
    
    switch (update)
    {
    case stay:
        ue_k.Serv_cell = new_bs.Name;       

        //std::cout << "[Debug] (stay) ue Serv Cell : " << new_bs.Name << "\n";

        //std::cout << "[Debug] ue  NR Cell : \n";  
 
        for(const auto& s : slice_provide_map)
            ue_k.Slice_prb_used[s.first] = s.second;
        for(const auto&s: slice_provide_map){
            
            //std::cout << "[Ken Debug]  PRB Using is  : "<< s.first << ", prb is " << s.second << "\n";

            //std::cout << "[Ken Debug]  capacity is   : "<< new_bs.Slice_capacity[s.first] << "\n";

            new_bs.Slice_load[s.first] +=  s.second ;
            //std::cout << "[Ken Debug]  slice load is   : "<< new_bs.Slice_load[s.first] << "\n";
            new_bs.Slice_capacity[s.first] = new_bs.Slice_prb_avail[s.first] - new_bs.Slice_load[s.first] ; 
            new_bs.Slice_utilization[s.first] = (float)new_bs.Slice_load[s.first] / new_bs.Slice_prb_avail[s.first];
            //std::cout << "[Debug] slice is : "<< s.first <<  "  Slice_utilization is : "     << new_bs.Slice_utilization[s.first] << "\n"; 

        }
        break;
    case switch_to_nr:
        ue_k.HO_cell = ue_k.Serv_cell;
        ue_k.NR_cell.push_back(ue_k.Serv_cell);
        ue_k.Serv_cell = new_bs.Name;
 
        for(const auto& s : slice_provide_map)
            ue_k.Slice_prb_used[s.first] = s.second;
        

        // delete target cell in NR fields     
        for(int n=0;n<ue_k.NR_cell.size();n++){
            if(ue_k.NR_cell[n]==new_bs.Name){
                ue_k.NR_cell.erase(ue_k.NR_cell.begin()+n) ;
                break;
            }
        }
        //std::cout << "[Debug] (switch_to_nr)ue new Serv Cell : " << new_bs.Name << "\n";
        
        //std::cout << "[Debug] ue new NR Cell : \n";
 
        for(const auto&s: slice_provide_map){

            //std::cout << "[Ken Debug]  PRB Using is  : "<< s.first << ", prb is " << s.second << "\n";

            //std::cout << "[Ken Debug]  Avail is   : "<< new_bs.Slice_prb_avail[s.first] << "\n";
        
            new_bs.Slice_load[s.first] +=  s.second ;
            new_bs.Slice_capacity[s.first] = new_bs.Slice_prb_avail[s.first] - new_bs.Slice_load[s.first] ; 
            new_bs.Slice_utilization[s.first] = (float)new_bs.Slice_load[s.first] / new_bs.Slice_prb_avail[s.first];
            //std::cout << "[Debug] slice is : "<< s.first <<  "  Slice_utilization is : "     << new_bs.Slice_utilization[s.first] << "\n"; 
        }
        break;
    case  handover_ue_k2:
        ue_k.HO_cell = ue_k.Serv_cell;
        //std::cout << "[Debug] handover : offload all the prb \n" ;
        for(const auto&s: slice_provide_map){
            new_bs.Slice_load[s.first] -=  s.second ;
            new_bs.Slice_capacity[s.first] = new_bs.Slice_prb_avail[s.first] - new_bs.Slice_load[s.first] ; 
            new_bs.Slice_utilization[s.first] = (float)new_bs.Slice_load[s.first] / new_bs.Slice_prb_avail[s.first];
            //std::cout << "[Debug] (offload) slice is : "<< s.first <<  "  Slice_load is : "     << new_bs.Slice_load[s.first] << "\n"; 
        }        
        break;
    default:
        break;
    }
}
 

void prediction_callback( Message& mbuf, int mtype, int subid, int len, Msg_component payload,  void* data ) {

  time_t now;
  string str_now;
  static unsigned int seq_number = 0; // static counter, not thread-safe

  int response_to = 0;	 // max timeout wating for a response

  int send_mtype = 0;
  int rmtype;							// received message type
  int delay = 1000000;		// mu-sec delay; default 1s

  string json ((char *)payload.get(), len); // RMR payload might not have a nil terminanted char

  cout << "[INFO] Prediction Callback got a message, type=" << mtype << ", length=" << len << "\n";
  cout << "[INFO] Payload is " << json << endl;

  PredictionHandler handler;
  try {
    Reader reader;
    StringStream ss(json.c_str());
    reader.Parse(ss,handler);
  } catch (...) {
    cout << "[ERROR] Got an exception on stringstream read parse\n";
  }

  // We are only considering download throughput
  unordered_map<string, int> throughput_map = handler.cell_pred_down;

  // Decision about CONTROL message
  // (1) Identify UE Id in Prediction message
  // (2) Iterate through Prediction message.
  //     If one of the cells has a higher throughput prediction than serving cell, send a CONTROL request
  //     We assume the first cell in the prediction message is the serving cell


  //----------------------------------------------------------------------
  // Ken start project
  //----------------------------------------------------------------------	
  mdclog_write(MDCLOG_INFO, "Prediction Callback got the message, start to query influxdb");

  std::vector<UE> UE_Group;
  std::vector<Cell> Cell_Group;
  std::vector<std::string> Slice_list;    

  
  
  
  // get the data , and store it to the struct defined in UE & Cell
  query_influxdb(UE_Group, Cell_Group, Slice_list);
  


  // do slice allocation

  clock_t begin = clock();
  slice_allocation(UE_Group, Cell_Group, Slice_list);
  clock_t end = clock();

  // output the outcome of data structure of UE and Cell 
  // to the file , "outcome_cell.csv" and "outcome_ue.csv"
  print_all_group(UE_Group, Cell_Group, Slice_list);

    

  printf("******************************************************Calculate Aver BS utilization******************************************************\n");
  //----------------------------------------------------------------------
  // Ken Calculate Average utilization
  // Calculate the sum of each cell's utilization
  // and divide by how many cell is serving ue
  //----------------------------------------------------------------------	
  std::unordered_map<std::string,float> Aver_BS_utilization;
  std::vector<std::string> Aver_BS;
  float Aver_BS_utilization_temp;

  // iterate all ue to get their serving cell's utilization
  for(int k=0;k<UE_Group.size();k++){
      //std::cout << "UE is : " << UE_Group[k].Name <<"\n";
      //auto ite = Aver_BS.find(UE_Group[k].Serv_cell);
      for(const auto& s: UE_Group[k].serv_slice_utilization){
          Aver_BS_utilization_temp = 0 ; // total utilization of a cell
          if(std::find(Aver_BS.begin(),Aver_BS.end(),UE_Group[k].Serv_cell) == Aver_BS.end()){
              Aver_BS_utilization_temp += s.second;
              //std::cout << "Serv is : " << UE_Group[k].Serv_cell << ", usage is : "<< s.second << "\n";
              Aver_BS_utilization[UE_Group[k].Serv_cell] += Aver_BS_utilization_temp*1/3; // three slice 
          }
      }            
      Aver_BS.push_back(UE_Group[k].Serv_cell);    
  }
       

  int it_cnt = 0 ;
  float Tot_BS_utilization = 0;
  for(const auto  it: Aver_BS_utilization){
      Tot_BS_utilization += it.second;
      it_cnt++;
      //std::cout  << it.first << ":" << it.second << "\n";    
      
  } 
    
  // store it in the file ,   "outcome.csv"
  string line;
  string outcome = "outcome.csv";

  ifstream fin(outcome.c_str(), std::ios::in);
  if(fin.good()){
      stringstream buffer;
      while (getline(fin, line)) buffer<<line+",\n";

      ofstream fout(outcome.c_str(), std::ios::out);
      buffer << Tot_BS_utilization/it_cnt << "," << (double)(end - begin) / CLOCKS_PER_SEC << endl ;
      fout << buffer.rdbuf();
  }  
    
  printf("******************************************************aver BS utilization = %f******************************************************\n", Tot_BS_utilization/it_cnt);
  //printf("**************************************Counter = %d**************************************", it_cnt);
  printf("******************************************************elapsed    time     = %f******************************************************\n", (double)(end - begin) / CLOCKS_PER_SEC);



  // write influxdb with a point
  for(int i=0 ; i < UE_Group.size() ; i++){
      db_influx->write(influxdb::Point{"slice_usage"}
      //.addTag("ue-id", UE_Group[i].Name  )   
      .addField("ueid", UE_Group[i].Name  )   
      .addField("serv_cell"  , UE_Group[i].Serv_cell  )   
      .addField("nr_cell_0", UE_Group[i].NR_cell[0]  )
      .addField("nr_cell_1", UE_Group[i].NR_cell[1]  )
      .addField("nr_cell_2", UE_Group[i].NR_cell[2]  )
      .addField("nr_cell_3", UE_Group[i].NR_cell[3]  )
      .addField("nr_cell_4", UE_Group[i].NR_cell[4]  )
      .addField("cell_hands_over",  UE_Group[i].HO_cell )
      .addField("serv_cell_slice_usage_urllc",  UE_Group[i].serv_slice_utilization["urllc"] )  
      .addField("nr_cell_slice_usage_0_urllc", UE_Group[i].NR_Slice_utilization[0]["urllc"]  )
      .addField("nr_cell_slice_usage_1_urllc", UE_Group[i].NR_Slice_utilization[1]["urllc"]  )
      .addField("nr_cell_slice_usage_2_urllc", UE_Group[i].NR_Slice_utilization[2]["urllc"]  )
      .addField("nr_cell_slice_usage_3_urllc", UE_Group[i].NR_Slice_utilization[3]["urllc"]  )
      .addField("nr_cell_slice_usage_4_urllc", UE_Group[i].NR_Slice_utilization[4]["urllc"]  )
      .addField("serv_cell_slice_usage_mmtc",  UE_Group[i].serv_slice_utilization["mmtc"] ) 
      .addField("nr_cell_slice_usage_0_mmtc", UE_Group[i].NR_Slice_utilization[0]["mmtc"]  )
      .addField("nr_cell_slice_usage_1_mmtc", UE_Group[i].NR_Slice_utilization[1]["mmtc"]  )
      .addField("nr_cell_slice_usage_2_mmtc", UE_Group[i].NR_Slice_utilization[2]["mmtc"]  )
      .addField("nr_cell_slice_usage_3_mmtc", UE_Group[i].NR_Slice_utilization[3]["mmtc"]  )
      .addField("nr_cell_slice_usage_4_mmtc", UE_Group[i].NR_Slice_utilization[4]["mmtc"]  )
      .addField("serv_cell_slice_usage_embb",  UE_Group[i].serv_slice_utilization["embb"] )
      .addField("nr_cell_slice_usage_0_embb", UE_Group[i].NR_Slice_utilization[0]["embb"]  )
      .addField("nr_cell_slice_usage_1_embb", UE_Group[i].NR_Slice_utilization[1]["embb"]  )
      .addField("nr_cell_slice_usage_2_embb", UE_Group[i].NR_Slice_utilization[2]["embb"]  )
      .addField("nr_cell_slice_usage_3_embb", UE_Group[i].NR_Slice_utilization[3]["embb"]  )
      .addField("nr_cell_slice_usage_4_embb", UE_Group[i].NR_Slice_utilization[4]["embb"]  )
      );
  
  
  }
  

  int serving_cell_throughput = 0;
  int highest_throughput = 0;
  string highest_throughput_cell_id;

  // Getting the current serving cell throughput prediction
  auto cell = throughput_map.find( handler.serving_cell_id );
  serving_cell_throughput = cell->second;

   // Iterating to identify the highest throughput prediction
  for (auto map_iter = throughput_map.begin(); map_iter != throughput_map.end(); map_iter++) {

    string curr_cellid = map_iter->first;
    int curr_throughput = map_iter->second;

    if ( highest_throughput < curr_throughput ) {
      highest_throughput = curr_throughput;
      highest_throughput_cell_id = curr_cellid;
    }

  }

  if ( highest_throughput > serving_cell_throughput ) {
    // building a handoff control message
    now = time( nullptr );
    str_now = ctime( &now );
    str_now.pop_back(); // removing the \n character

    seq_number++;       // static counter, not thread-safe

    rapidjson::StringBuffer s;
	  rapidjson::PrettyWriter<rapidjson::StringBuffer> writer(s);
    writer.StartObject();
    writer.Key( "command" );
    writer.String( "HandOff" );
    writer.Key( "seqNo" );
    writer.Int( seq_number );
    writer.Key( "ue" );
    writer.String( handler.ue_id.c_str() );
    writer.Key( "fromCell" );
    writer.String( handler.serving_cell_id.c_str() );
    writer.Key( "toCell" );
    writer.String( highest_throughput_cell_id.c_str() );
    writer.Key( "timestamp" );
    writer.String( str_now.c_str() );
    writer.Key( "reason" );
    writer.String( "HandOff Control Request from TS xApp" );
    writer.Key( "ttl" );
    writer.Int( 10 );
    writer.EndObject();
    // creates a message like
    /* {
      "command": "HandOff",
      "seqNo": 1,
      "ue": "ueid-here",
      "fromCell": "CID1",
      "toCell": "CID3",
      "timestamp": "Sat May 22 10:35:33 2021",
      "reason": "HandOff Control Request from TS xApp",
      "ttl": 10
    } */

    // sending a control request message
    if ( ts_control_api == TsControlApi::REST ) {
      send_rest_control_request( s.GetString() );
    } else {
      send_grpc_control_request();
    }

  } else {
    cout << "[INFO] The current serving cell \"" << handler.serving_cell_id << "\" is the best one" << endl;
  }

  // mbuf.Send_response( 101, -1, 5, (unsigned char *) "OK1\n" );	// validate that we can use the same buffer for 2 rts calls
  // mbuf.Send_response( 101, -1, 5, (unsigned char *) "OK2\n" );
}

void send_prediction_request( vector<string> ues_to_predict ) {

  std::unique_ptr<Message> msg;
  Msg_component payload;                                // special type of unique pointer to the payload

  int sz;
  int i;
  size_t plen;
  Msg_component send_payload;

  msg = xfw->Alloc_msg( 2048 );

  sz = msg->Get_available_size();  // we'll reuse a message if we received one back; ensure it's big enough
  if( sz < 2048 ) {
    fprintf( stderr, "[ERROR] message returned did not have enough size: %d [%d]\n", sz, i );
    exit( 1 );
  }

  string ues_list = "[";

  for (int i = 0; i < ues_to_predict.size(); i++) {
    if (i == ues_to_predict.size() - 1) {
      ues_list = ues_list + "\"" + ues_to_predict.at(i) + "\"]";
    } else {
      ues_list = ues_list + "\"" + ues_to_predict.at(i) + "\"" + ",";
    }
  }

  string message_body = "{\"UEPredictionSet\": " + ues_list + "}";

  send_payload = msg->Get_payload(); // direct access to payload
  snprintf( (char *) send_payload.get(), 2048, "%s", message_body.c_str() );

  plen = strlen( (char *)send_payload.get() );

  cout << "[INFO] Prediction Request length=" << plen << ", payload=" << send_payload.get() << endl;

  // payload updated in place, nothing to copy from, so payload parm is nil
  if ( ! msg->Send_msg( TS_UE_LIST, Message::NO_SUBID, plen, NULL )) { // msg type 30000
    fprintf( stderr, "[ERROR] send failed: %d\n", msg->Get_state() );
  }

}

/* This function works with Anomaly Detection(AD) xApp. It is invoked when anomalous UEs are send by AD xApp.
 * It parses the payload received from AD xApp, sends an ACK with same UEID as payload to AD xApp, and
 * sends a prediction request to the QP Driver xApp.
 */
void ad_callback( Message& mbuf, int mtype, int subid, int len, Msg_component payload,  void* data ) {
  string json ((char *)payload.get(), len); // RMR payload might not have a nil terminanted char

  cout << "[INFO] AD Callback got a message, type=" << mtype << ", length=" << len << "\n";
  cout << "[INFO] Payload is " << json << "\n";

  AnomalyHandler handler;
  Reader reader;
  StringStream ss(json.c_str());
  reader.Parse(ss,handler);

  // just sending ACK to the AD xApp
  mbuf.Send_response( TS_ANOMALY_ACK, Message::NO_SUBID, len, nullptr );  // msg type 30004

  // TODO should we use the threshold received in the A1_POLICY_REQ message and compare with Degradation in TS_ANOMALY_UPDATE?
  // if( handler.degradation < rsrp_threshold )
  send_prediction_request(handler.prediction_ues);
}

extern int main( int argc, char** argv ) {

 
 int nthreads = 1;
  char*	port = (char *) "4560";
  shared_ptr<grpc::Channel> channel;

  Config *config = new Config();
  string api = config->Get_control_str("ts_control_api");
  ts_control_ep = config->Get_control_str("ts_control_ep");
  if ( api.empty() ) {
    cout << "[ERROR] a control api (rest/grpc) is required in xApp descriptor\n";
    exit(1);
  }
  if ( api.compare("rest") == 0 ) {
    ts_control_api = TsControlApi::REST;
  } else {
    ts_control_api = TsControlApi::gRPC;
  }

  channel = grpc::CreateChannel(ts_control_ep, grpc::InsecureChannelCredentials());
  rc_stub = api::MsgComm::NewStub(channel, grpc::StubOptions());

  fprintf( stderr, "[TS xApp] listening on port %s\n", port );
  xfw = std::unique_ptr<Xapp>( new Xapp( port, true ) );

  xfw->Add_msg_cb( A1_POLICY_REQ, policy_callback, NULL );          // msg type 20010
  xfw->Add_msg_cb( TS_QOE_PREDICTION, prediction_callback, NULL );  // msg type 30002
  xfw->Add_msg_cb( TS_ANOMALY_UPDATE, ad_callback, NULL ); /*Register a callback function for msg type 30003*/

  xfw->Run( nthreads );

}

