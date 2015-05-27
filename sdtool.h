/*
 * sdtool.h
 *
 *  Created on: 2014-3-20
 *      Author: mmk
 */

#ifndef SDTOOL_H_
#define SDTOOL_H_
//#include <map>
#include <vector>
#include <string>
#include <sstream>

using namespace std;

typedef struct {
	char *name;
	char *value;
}RKSdBootCfgItem;
typedef struct{
	string strKey;
	string strValue;
}STRUCT_SD_CONFIG_ITEM,*PSTRUCT_SD_CONFIG_ITEM;
typedef vector<STRUCT_SD_CONFIG_ITEM> VEC_SD_CONFIG;
//typedef map<string,string> MAP_SD_CONFIG;
//typedef MAP_SD_CONFIG::iterator sd_config_map_iter;



#endif /* SDTOOL_H_ */
