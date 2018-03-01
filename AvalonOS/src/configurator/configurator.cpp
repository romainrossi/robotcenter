/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <libgen.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

#include <algorithm>
#include <fstream>
#include <iostream>

#include <stout/foreach.hpp>
#include <stout/os.hpp>
#include <stout/strings.hpp>

#include "configurator.hpp"
#include "configuration.hpp"

using namespace avalon::internal;

using std::ifstream;
using std::map;
using std::string;
using std::vector;


const char* Configurator::DEFAULT_CONFIG_DIR = "conf";
const char* Configurator::CONFIG_FILE_NAME = "avalon.conf";
const char* Configurator::ENV_VAR_PREFIX = "AVALON_";


// Define a function for accessing the list of environment variables
// in a platform-independent way.
// On Mac OS X, the environ symbol isn't visible to shared libraries,
// so we must use the _NSGetEnviron() function (see man environ on OS X).
// On other platforms, it's fine to access environ from shared libraries.
#ifdef __APPLE__
#include "crt_externs.h"
namespace {
char** getEnviron() { return *_NSGetEnviron(); }
}
#else
extern char** environ;
namespace {
char** getEnviron() { return environ; }
}
#endif /* __APPLE__ */


void Configurator::validate()
{
  foreachpair (const string& key, const ConfigOption& opt, options) {
    if (conf.contains(key) && opt.validator &&
        !opt.validator->isValid(conf[key])) {
      string msg = "Invalid value for '" + key + "' option: " + conf[key];
      throw ConfigurationException(msg.c_str());
    }
  }
}


Configuration& Configurator::load(int argc, char** argv)
{
  loadEnv();
  loadCommandLine(argc, argv);
  loadConfigFileIfGiven();
  loadDefaults();
  validate();
  return conf;
}


Configuration& Configurator::load()
{
  loadEnv();
  loadConfigFileIfGiven();
  loadDefaults();
  validate();
  return conf;
}


Configuration& Configurator::load(const map<string, string>& _params) 
{
  loadEnv();
  conf.loadMap(_params);
  loadConfigFileIfGiven();
  loadDefaults();
  validate();
  return conf;
}


void Configurator::loadConfigFileIfGiven(bool overwrite) {
  if (conf.contains("conf")) {
    // If conf param is given, always look for a config file in that directory.
    Try<string> path =
      os::realpath(conf["conf"] + "/" + CONFIG_FILE_NAME);

    if (path.isError()) {
      LOG(WARNING) << "Cannot load config file: " << path.error();
    } else {
      loadConfigFile(path.get(), overwrite);
    }
  }
}


void Configurator::loadEnv(bool overwrite)
{
  char** environ = getEnviron();
  int i = 0;
  while (environ[i] != NULL) {
    string line = environ[i];
    if (line.find(ENV_VAR_PREFIX) == 0) {
      string key, val;
      size_t eq = line.find_first_of("=");
      if (eq == string::npos) 
        continue; // ignore malformed lines (they shouldn't occur in environ!)
      key = line.substr(strlen(ENV_VAR_PREFIX), eq - strlen(ENV_VAR_PREFIX));
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      val = line.substr(eq + 1);
      // Disallow setting home through the environment, because it should
      // always be resolved from the running Avalon binary (if any)
      if ((overwrite || !conf.contains(key))) {
        conf[key] = val;
      }
    }
    i++;
  }
}


void Configurator::loadCommandLine(int argc,
                                   char** argv,
                                   bool overwrite)
{
  // Convert args 1 and above to STL strings
  vector<string> args;
  for (int i=1; i < argc; i++) {
    args.push_back(string(argv[i]));
  }

  // Remember number of times we see each key to warn the user if we see a
  // key multiple times (since right now we only use the first value)
  map<string, int> timesSeen;

  for (size_t i = 0; i < args.size(); i++) {
    string key, val;
    bool set = false;
    if (args[i].find("--", 0) == 0) {
      // handle "--" case
      size_t eq = args[i].find_first_of("=");
      if (eq == string::npos &&
          args[i].find("--no-", 0) == 0) { // handle --no-blah
        key = args[i].substr(5);
        val = "0";
        set = true;
        checkCommandLineParamFormat(key, true);
      } else if (eq == string::npos) {     // handle --blah
        key = args[i].substr(2);
        val = "1";
        set = true;
        checkCommandLineParamFormat(key, true);
      } else {                             // handle --blah=25
        key = args[i].substr(2, eq-2); 
        val = args[i].substr(eq+1);
        set = true;
        checkCommandLineParamFormat(key, false);
      }
    } else if (args[i].find_first_of("-", 0) == 0 && 
               args[i].size() > 1) { 
      // handle "-" case
      char shortName = '\0';
      if (args[i].find("-no-",0) == 0 && args[i].size() == 5) {
        shortName = args[i][4];
      } else if (args[i].size() == 2) {
        shortName = args[i][1];
      }
      if (shortName == '\0' || getLongName(shortName) == "") {
        string message = "Short option '" + args[i] + "' unrecognized ";
        throw ConfigurationException(message.c_str());
      }
      key = getLongName(shortName);
      if (args[i].find("-no-",0) == 0) { // handle -no-b
        val = "0";
        set = true;
        checkCommandLineParamFormat(key, true);
      } else if (options[key].validator->isBool() ||
                 i+1 == args.size() ) {  // handle -b
        val = "1";
        set = true;
        checkCommandLineParamFormat(key, true);
      } else {                           // handle -b 25
        val = args[i+1];
        set = true;
        i++;  // we've consumed next parameter as a "value"-parameter
      }
    }
    std::transform(key.begin(), key.end(), key.begin(), ::tolower);
    // Check whether the key has already appeared in the command line
    timesSeen[key]++;
    if (timesSeen[key] == 2) {
      LOG(WARNING) << "\"" << key << "\" option appears multiple times in "
                   << "command line; only the first value will be used";
    }
    if (set && (overwrite || !conf.contains(key)) && timesSeen[key] == 1) {
      conf[key] = val;
    }
  }
}


void Configurator::loadConfigFile(const string& fname, bool overwrite) 
{
  ifstream cfg(fname.c_str(), std::ios::in);
  if (!cfg.is_open()) {
    string message = "Couldn't read Avalon config file: " + fname;
    throw ConfigurationException(message.c_str());
  }

  // Remember number of times we see each key to warn the user if we see a
  // key multiple times (since right now we only use the first value)
  map<string, int> timesSeen;

  string line, originalLine;

  while (!cfg.eof()) {
    getline(cfg, line);
    originalLine = line;
    // Strip any comment at end of line
    size_t hash = line.find_first_of("#"); // strip comments
    if (hash != string::npos) {
      line = line.substr(0, hash);
    }
    // Check for empty line
    line = strings::trim(line);
    if (line == "") {
      continue;
    }
    // Split line by = and trim to get key and value
    vector<string> tokens = strings::tokenize(line, "=");
    if (tokens.size() != 2) {
      string message = "Malformed line in config file: '" +
                       strings::trim(originalLine) + "'";
      throw ConfigurationException(message.c_str());
    }
    string key = strings::trim(tokens[0]);
    string value = strings::trim(tokens[1]);
    // Check whether the key has already appeared in this config file
    timesSeen[key]++;
    if (timesSeen[key] == 2) {
      LOG(WARNING) << "\"" << key << "\" option appears multiple times in "
                   << fname << "; only the first value will be used";
    }
    if ((overwrite || !conf.contains(key)) && timesSeen[key] == 1) {
      conf[key] = value;
    }
  }
  cfg.close();
}


string Configurator::getUsage() const 
{
  const int PAD = 5;
  string usage;

  map<string,string> col1; // key -> col 1 string
  size_t maxLen = 0;

  // construct string for the first column and get size of column
  foreachpair (const string& key, const ConfigOption& opt, options) {
    string val;
    if (opt.validator->isBool())
      val = "  --[no-]" + key;
    else
      val = "  --" + key + "=VAL";

    if (opt.hasShortName) {
      if (opt.validator->isBool()) 
        val += string(" (or -[no-]") + opt.shortName + ")";
      else
        val += string(" (or -") + opt.shortName + " VAL)";
    }

    col1[key] = val;
    maxLen = val.size() > maxLen ? val.size() : maxLen;
  }

  foreachpair (const string& key, const ConfigOption& opt, options) {
    string helpStr = opt.helpString;
    string line = col1[key];

    if (opt.defaultValue != "") {  // add default value
      // Place a space between help string and (default: VAL) if the
      // help string does not end with a newline itself
      size_t lastNewLine = helpStr.find_last_of("\n\r");
      if (helpStr.size() > 0 && lastNewLine != helpStr.size() - 1) {
        helpStr += " ";
      }
      string defval = opt.defaultValue;
      if (opt.validator->isBool())
        defval = opt.defaultValue == "0" ? "false" : "true";

      helpStr += "(default: " + defval + ")";
    }

    string pad(PAD + maxLen - line.size(), ' ');
    line += pad;
    size_t pos1 = 0, pos2 = 0;
    pos2 = helpStr.find_first_of("\n\r", pos1);
    line += helpStr.substr(pos1, pos2 - pos1) + "\n";
    usage += line;

    while(pos2 != string::npos) {  // handle multi line help strings
      line = "";
      pos1 = pos2 + 1;
      string pad2(PAD + maxLen, ' ');
      line += pad2;
      pos2 = helpStr.find_first_of("\n\r", pos1);
      line += helpStr.substr(pos1, pos2 - pos1) + "\n";
      usage += line;
    }

  }
  return usage;
}


void Configurator::loadDefaults()
{
  foreachpair (const string& key, const ConfigOption& option, options) {
    if (option.hasDefault && !conf.contains(key)) {
      conf[key] = option.defaultValue;
    }
  }
}


vector<string> Configurator::getOptions() const 
{
  vector<string> ret;
  foreachkey (const string& key, options) {
    ret.push_back(key);
  }
  return ret;
}


Configuration& Configurator::getConfiguration()
{
  return conf;
}

string Configurator::getLongName(char shortName) const
{
  foreachpair (const string& key, const ConfigOption& opt, options) {
    if (opt.hasShortName && opt.shortName == shortName)
      return key;
  }
  return "";
}


void Configurator::clearAvalonEnvironmentVars()
{
  char** environ = getEnviron();
  int i = 0;
  vector<string> toRemove;
  while (environ[i] != NULL) {
    string line = environ[i];
    if (line.find(ENV_VAR_PREFIX) == 0) {
      string key;
      size_t eq = line.find_first_of("=");
      if (eq == string::npos) 
        continue; // ignore malformed lines (they shouldn't occur in environ!)
      key = line.substr(strlen(ENV_VAR_PREFIX), eq - strlen(ENV_VAR_PREFIX));
      toRemove.push_back(key);
    }
    i++;
  }
  foreach (string& str, toRemove) {
    unsetenv(str.c_str());
  }
}


void Configurator::checkCommandLineParamFormat(const string& key, bool gotBool) 
  throw(ConfigurationException)
{
  if (options.find(key) != options.end() && 
      options[key].validator->isBool() != gotBool) {
    string message = "Option '" + key + "' should ";
    if (gotBool)
      message += "not ";
    message += "be a boolean.";

    throw ConfigurationException(message.c_str());
  }
}


void Configurator::dumpToGoogleLog()
{
  LOG(INFO) << "Dumping configuration options:";
  const map<string, string>& params = conf.getMap();
  foreachpair (const string& key, const string& val, params) {
    LOG(INFO) << "  " << key << " = " << val;
  }
  LOG(INFO) << "End configuration dump";
}
