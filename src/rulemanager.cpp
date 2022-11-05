#include "rulemanager.h"

RuleManager::RuleManager(const char* daemon_name, string rules_directory) {
	this->daemon_name = daemon_name;
	this->rules_directory = rules_directory.c_str();
	Logger::logInfo("Loading available rules from " + std::string(this->rules_directory));
	loadRules();
}

void RuleManager::loadRules() {

	const std::filesystem::path rules_dir{this->rules_directory};

	// load all files in this->rules_directory ending with ".conf"
	for(auto& file: fs::directory_iterator(rules_dir)) {
		if (file.path().u8string().find(".conf", 0) != string::npos) {
			generateRuleFromFile(std::string(file.path()));
		}
	}
}

// check if read values are valid
bool RuleManager::generateRuleFromFile(string filename) {
	Logger::logInfo("Loading file " +filename);
	RuleReturn file_content = readRuleFile(filename);

	// check if the rule file was readable and present
	if (!file_content.success) {
		Logger::logError("Unable to read this file! Please have a look at this rule.");
	}
	else {
		Logger::logInfo("  |-> Validating file ...");
		if (checkIfRuleIsValid(file_content.rule)) {


			Logger::logInfo("  |-> Registering rule ...");
			if (registerRule(file_content.rule)) {

				// register cgroups if enabled
				if (this->rules[file_content.rule["COMMAND"]].enable_limiting) {
					Logger::logInfo("  |-> Registering cgroup ...");
					createCgroup(&this->rules[file_content.rule["COMMAND"]]);
				}
			}
			else {
				Logger::logError(" '-> Unable to register rule! Error parsing rule settings.");
				return false;
			}

		}
		else {
			Logger::logError(" '-> Broken or incomplete rule! Skipping.");
			Logger::logDebug("Make sure all mandatory settings are present and correct datatypes are used.");
			return false;
		}
	}

	// showRuleContent(file_content.rule);
	Logger::logInfo("  '-> Done!");
	return true;
}

bool RuleManager::registerRule(map<string, string> file_content) {

	try {

		Rule rule;

		// mandatory rule settings first
		rule.rule_name = file_content["RULE_NAME"];
		rule.command = file_content["COMMAND"];
		rule.cpu_trigger_threshold = stod(file_content["CPU_TRIGGER_THRESHOLD"]);
		rule.mem_trigger_threshold = stod(file_content["MEM_TRIGGER_THRESHOLD"]);

		// check if values are valid (e.g. not negative values)
		if (!file_content["CHECKS_BEFORE_ALERT"].empty())
			rule.checks_before_alert = stoi(file_content["CHECKS_BEFORE_ALERT"]);
			if (rule.checks_before_alert < 0)
				return false;

		if (!file_content["LIMIT_CPU_PERCENT"].empty())
			rule.limit_cpu_percent = stoi(file_content["LIMIT_CPU_PERCENT"]);
			if (rule.limit_cpu_percent < 0 || rule.limit_cpu_percent > 100)
				return false;

		if (!file_content["LIMIT_MEMORY_VALUE"].empty())
			rule.limit_memory_value = stoi(file_content["LIMIT_MEMORY_VALUE"]);
			if (rule.limit_memory_value < 0)
				return false;

		// optional rule settings
		(file_content["NO_CHECK"] == "1") ? rule.no_check = true : rule.no_check = false;
		(file_content["FREEZE"] == "1") ? rule.freeze = true : rule.freeze = false;
		(file_content["OOM_KILL_ENABLED"] == "1") ? rule.oom_kill_enabled = true : rule.oom_kill_enabled = false;
		(file_content["PID_KILL_ENABLED"] == "1") ? rule.pid_kill_enabled = true : rule.pid_kill_enabled = false;
		(file_content["SEND_PROCESS_FILES"] == "1") ? rule.send_process_files = true : rule.send_process_files = false;
		(file_content["ENABLE_LIMITING"] == "1") ? rule.enable_limiting = true : rule.enable_limiting = false;

		// cgroup name (daemon_name+'-'+rule_name)
		std::string cgroup_name = this->daemon_name;
		cgroup_name += "-";
		cgroup_name += rule.rule_name;
		rule.cgroup_name = cgroup_name;

		// cgroup root dir
		std::string cgroup_root_dir = this->cgroup_root_dir;
		cgroup_root_dir += "/";
		cgroup_root_dir += cgroup_name;
		rule.cgroup_root_dir = cgroup_root_dir;

		// create all needed file references for this cgroup
		rule.cgroup_subtree_control_file = cgroup_root_dir+"/"+this->subtree_control_file;
		rule.cgroup_cpu_max_file = cgroup_root_dir+"/"+this->cpu_max_file;
		rule.cgroup_procs_file = cgroup_root_dir+"/"+this->procs_file;
		rule.cgroup_memory_high_file = cgroup_root_dir+"/"+this->memory_high_file;
		rule.cgroup_memory_max_file = cgroup_root_dir+"/"+this->memory_max_file;
		rule.cgroup_freezer_file = cgroup_root_dir+"/"+this->freezer_file;

		// register this rule to the global rulemanager
		this->rules.insert(std::pair<string, Rule>(rule.command, rule));

		return true;

	} catch (...) {
		return false;
	}

}

bool RuleManager::checkIfRuleIsValid(map<string, string> file_content) {

	// check if all mandatory_rule_settings are set, otherwise discard this rule
	for (auto& s : this->mandatory_rule_settings) {
		if (file_content[s].empty())
			return false;
	}

	// check that all settings do have the correct datatype
	set<string> boolean_settings = {
		file_content["NO_CHECK"],
		file_content["FREEZE"],
		file_content["OOM_KILL_ENABLED"],
		file_content["PID_KILL_ENABLED"],
		file_content["SEND_PROCESS_FILES"],
		file_content["ENABLE_LIMITING"]
	};
	for (auto& b : boolean_settings) {
		if ((!b.empty()) && (b != "1" && b != "0")) {
			return false;
		}
	}

	// check if value is double
	set<string> double_settings = {
		file_content["CPU_TRIGGER_THRESHOLD"],
		file_content["MEM_TRIGGER_THRESHOLD"]
	};
	for (auto& d : double_settings) {
		if ((!d.empty()) && (d.find_first_not_of(".0123456789") != std::string::npos)) {
			return false;
		}
	}

	// check if value is int
	set<string> int_settings = {
		file_content["CHECKS_BEFORE_ALERT"],
		file_content["LIMIT_MEMORY_VALUE"],
		file_content["LIMIT_CPU_PERCENT"]
	};
	for (auto& i : int_settings) {
		if ((!i.empty()) && (i.find_first_not_of("0123456789") != std::string::npos)) {
			return false;
		}
	}

	// return true if all checks are passed
	return true;
}

bool RuleManager::createCgroup(Rule* rule) {

	// at least one cgroup setting must be set otherwise rule is broken
	if ( rule->limit_cpu_percent >= 0 || rule->limit_memory_value >= 0 ||  rule->freeze || rule->oom_kill_enabled || rule->pid_kill_enabled) {
		if (Logger::getLogLevel() == "debug") {
			Logger::logDebug("limit_cpu_percent: "+to_string(rule->limit_cpu_percent));
			Logger::logDebug("limit_memory_value: "+to_string(rule->limit_memory_value));
			Logger::logDebug("oom_kill_enabled: "+to_string(rule->oom_kill_enabled));
			Logger::logDebug("pid_kill_enabled: "+to_string(rule->pid_kill_enabled));
			Logger::logDebug("freezer: "+to_string(rule->freeze));
		}

		// check if the cgroup already exists, otherwise create it
		if (fs::exists(rule->cgroup_root_dir.c_str())) {
			Logger::logInfo("  |-> Cgroup "+rule->cgroup_root_dir+" already exists!");
		}
		else {
			if (mkdir(rule->cgroup_root_dir.c_str(), 0750) != -1) {
				Logger::logInfo("  |-> Created cgroup "+rule->cgroup_root_dir);
			}
			else {
				Logger::logError("Unable to create cgroup "+std::string(rule->cgroup_root_dir));
				return false;
			}
		}

		// write all needed values into the cgroup controller files
		if (!writeInFile(rule->cgroup_subtree_control_file, "+pids +cpu +cpuset +memory")) {
			Logger::logError("Something went wrong while modifying "+rule->cgroup_subtree_control_file);
			return false;
		}

		// prepare the freezer file for the given cgroup
		string freeze;
		if (rule->freeze) {	freeze = "1"; }
		else { freeze = "0"; }
		if (!writeInFile(rule->cgroup_freezer_file, freeze)) {
			Logger::logError("Something went wrong while modifying "+rule->cgroup_freezer_file);
			return false;
		}

		// prepare the cpu.max file for the given cgroup
		string cpu_max;
		if (rule->limit_cpu_percent > 0) { cpu_max = to_string(rule->limit_cpu_percent)+"000 100000"; }
		else { cpu_max = "max 100000"; }
		if (!writeInFile(rule->cgroup_cpu_max_file, cpu_max)) {
			Logger::logError("Something went wrong while modifying "+rule->cgroup_cpu_max_file);
			return false;
		}

		// prepare the memory.high and memory.max file for the given cgroup
		string memory_value;
		if (rule->limit_memory_value > 0) { memory_value = to_string(rule->limit_memory_value); }
		else { memory_value = "max"; }
		if (!writeInFile(rule->cgroup_memory_high_file, memory_value)) {
			Logger::logError("Something went wrong while modifying "+rule->cgroup_memory_high_file);
			return false;
		}
		if (rule->oom_kill_enabled) {
			if (!writeInFile(rule->cgroup_memory_max_file, memory_value)) {
				Logger::logError("Something went wrong while modifying "+rule->cgroup_memory_max_file);
				return false;
			}
		}

	}

	return true;
}

bool RuleManager::writeInFile(string filename, string text) {
	try {
		FILE* file = fopen(filename.c_str(), "w+");
		if (file) {
			fprintf(file, text.c_str());
			fclose(file);
		}
		else
			return false;
		return true;
	} catch (...) {
		Logger::logError("Unable to write to file "+filename+"!");
		return false;
	}
}

Rule* RuleManager::loadIfRuleExists(string command) {
	for (auto& r : this->rules) {
		if (command.find(r.first) != std::string::npos){
			return &this->rules[r.first];
		}
	}
	return nullptr;
}

// read the file and return a map object with read values
RuleManager::RuleReturn RuleManager::readRuleFile(string filename) {
	RuleReturn current_rule_content;
	map<string, string> rule;
	fstream rules_file;
	rules_file.open(filename, ios::in);
	if (!rules_file) {
		Logger::logError("Rule file " + filename + " is not present or readable!");
		current_rule_content.success = false;
		current_rule_content.rule = rule;
	}
	else {
		if (rules_file.is_open()) {
			string line;
			while(getline(rules_file, line)) {
				if(this->available_rule_settings.find(line.substr(0, line.find("="))) != this->available_rule_settings.end()) {
					rule.insert(std::pair<string,string>(line.substr(0, line.find("=")),line.substr(line.find("=")+1)));
				}
			}
			rules_file.close();
		}
		current_rule_content.success = true;
		current_rule_content.rule = rule;
	}
	return current_rule_content;
}

// for debug purpose only
void RuleManager::showRuleContent(map<string, string> rule) {
	for (auto& s : rule)
		std::cout << s.first << " -> " << s.second << '\n';
}