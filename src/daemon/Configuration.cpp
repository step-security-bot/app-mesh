#include <set>
#include <unistd.h> //environ

#include <ace/Signal.h>
#include <boost/algorithm/string_regex.hpp>

#include "Configuration.h"
#include "Label.h"
#include "ResourceCollection.h"
#include "application/Application.h"
#include "consul/ConsulConnection.h"
#include "rest/PrometheusRest.h"
#include "rest/RestHandler.h"
#include "security/Security.h"
#include "security/User.h"

#include "../common/DateTime.h"
#include "../common/DurationParse.h"
#include "../common/Utility.h"
#include "../common/os/pstree.hpp"

extern char **environ; // unistd.h

std::shared_ptr<Configuration> Configuration::m_instance = nullptr;
Configuration::Configuration()
	: m_scheduleInterval(DEFAULT_SCHEDULE_INTERVAL), m_disableExecUser(false)
{
	m_jsonFilePath = (fs::path(Utility::getParentDir()) / APPMESH_CONFIG_JSON_FILE).string();
	m_label = std::make_unique<Label>();
	m_rest = std::make_shared<JsonRest>();
	m_consul = std::make_shared<JsonConsul>();
	LOG_INF << "Configuration file <" << m_jsonFilePath << ">";
}

Configuration::~Configuration()
{
}

std::shared_ptr<Configuration> Configuration::instance()
{
	return m_instance;
}

void Configuration::instance(std::shared_ptr<Configuration> config)
{
	m_instance = config;
}

std::shared_ptr<Configuration> Configuration::FromJson(const std::string &str, bool applyEnv)
{
	nlohmann::json jsonValue;
	try
	{
		jsonValue = nlohmann::json::parse((str));
		if (applyEnv)
		{
			// Only the first time read from ENV
			Configuration::readConfigFromEnv(jsonValue);
		}
	}
	catch (const std::exception &e)
	{
		LOG_ERR << "Failed to parse configuration file with error <" << e.what() << ">";
		throw std::invalid_argument("Failed to parse configuration file, please check json configuration file format");
	}
	catch (...)
	{
		LOG_ERR << "Failed to parse configuration file with error <unknown exception>";
		throw std::invalid_argument("Failed to parse configuration file, please check json configuration file format");
	}
	auto config = std::make_shared<Configuration>();

	// Global Parameters
	config->m_hostDescription = GET_JSON_STR_VALUE(jsonValue, JSON_KEY_Description);
	config->m_defaultExecUser = GET_JSON_STR_VALUE(jsonValue, JSON_KEY_DefaultExecUser);
	config->m_disableExecUser = GET_JSON_BOOL_VALUE(jsonValue, JSON_KEY_DisableExecUser);
	config->m_defaultWorkDir = GET_JSON_STR_VALUE(jsonValue, JSON_KEY_WorkingDirectory);
	config->m_scheduleInterval = GET_JSON_INT_VALUE(jsonValue, JSON_KEY_ScheduleIntervalSeconds);
	config->m_logLevel = GET_JSON_STR_VALUE(jsonValue, JSON_KEY_LogLevel);

	unsigned int gid, uid;
	if (!config->m_defaultExecUser.empty() && !Utility::getUid(config->m_defaultExecUser, uid, gid))
	{
		LOG_ERR << "No such OS user: " << config->m_defaultExecUser;
		throw std::invalid_argument("No such OS user for default execution");
	}
	if (config->m_scheduleInterval < 1 || config->m_scheduleInterval > 100)
	{
		// Use default value instead
		config->m_scheduleInterval = DEFAULT_SCHEDULE_INTERVAL;
		LOG_INF << "Default value <" << config->m_scheduleInterval << "> will by used for ScheduleIntervalSec";
	}

	// REST
	if (HAS_JSON_FIELD(jsonValue, JSON_KEY_REST))
	{
		config->m_rest = JsonRest::FromJson(jsonValue.at(JSON_KEY_REST));
	}

	// Labels
	if (HAS_JSON_FIELD(jsonValue, JSON_KEY_Labels))
	{
		config->m_label = Label::FromJson(jsonValue.at(JSON_KEY_Labels));
		// add default label here
		config->m_label->addLabel(DEFAULT_LABEL_HOST_NAME, MY_HOST_NAME);
	}
	// Consul
	if (HAS_JSON_FIELD(jsonValue, JSON_KEY_CONSUL))
	{
		config->m_consul = JsonConsul::FromJson(jsonValue.at(JSON_KEY_CONSUL), config->getRestListenPort());
	}

	return config;
}

std::string Configuration::readConfiguration()
{
	const auto jsonPath = fs::path(Utility::getParentDir()) / APPMESH_CONFIG_JSON_FILE;
	return Utility::readFileCpp(jsonPath.string());
}

void SigHupHandler(int signo)
{
	const static char fname[] = "SigHupHandler() ";

	LOG_INF << fname << "Handle singal :" << signo;
	auto config = Configuration::instance();
	if (config != nullptr)
	{
		try
		{
			config->hotUpdate(nlohmann::json::parse(Configuration::readConfiguration()));
		}
		catch (const std::exception &e)
		{
			LOG_ERR << fname << e.what();
		}
		catch (...)
		{
			LOG_ERR << fname << "unknown exception";
		}
	}
}

void Configuration::handleSignal()
{
	static ACE_Sig_Action *sig_action = nullptr;
	if (!sig_action)
	{
		sig_action = new ACE_Sig_Action();
		sig_action->handler(SigHupHandler);
		sig_action->register_action(SIGHUP);
	}

	static ACE_Sig_Action *sig_pipe = nullptr;
	if (!sig_pipe)
	{
		sig_pipe = new ACE_Sig_Action((ACE_SignalHandler)SIG_IGN);
		sig_pipe->register_action(SIGPIPE, 0);
	}
}

nlohmann::json Configuration::AsJson(bool returnRuntimeInfo, const std::string &user, bool returnUnPersistApp)
{
	nlohmann::json result = nlohmann::json::object();
	// Applications
	result[JSON_KEY_Applications] = serializeApplication(false, user, returnUnPersistApp);

	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);

	// Global parameters
	result[JSON_KEY_Description] = std::string(m_hostDescription);
	result[JSON_KEY_DefaultExecUser] = std::string(m_defaultExecUser);
	result[JSON_KEY_DisableExecUser] = (m_disableExecUser);
	result[JSON_KEY_WorkingDirectory] = std::string(m_defaultWorkDir);
	result[JSON_KEY_ScheduleIntervalSeconds] = (m_scheduleInterval);
	result[JSON_KEY_LogLevel] = std::string(m_logLevel);

	// REST
	result[JSON_KEY_REST] = m_rest->AsJson();

	// Labels
	result[JSON_KEY_Labels] = m_label->AsJson();

	// Consul
	result[JSON_KEY_CONSUL] = m_consul->AsJson();

	// Build version
	result[JSON_KEY_VERSION] = std::string(__MICRO_VAR__(BUILD_TAG));

	return result;
}

std::vector<std::shared_ptr<Application>> Configuration::getApps() const
{
	std::lock_guard<std::recursive_mutex> guard(m_appMutex);
	return m_apps;
}

void Configuration::addApp2Map(std::shared_ptr<Application> app)
{
	const static char fname[] = "Configuration::addApp2Map() ";

	std::lock_guard<std::recursive_mutex> guard(m_appMutex);
	for (std::size_t i = 0; i < m_apps.size(); i++)
	{
		if (m_apps[i]->getName() == app->getName())
		{
			LOG_INF << fname << "Application <" << app->getName() << "> already exist.";
			return;
		}
	}
	m_apps.push_back(app);
}

int Configuration::getScheduleInterval()
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_scheduleInterval;
}

int Configuration::getRestListenPort()
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_rest->m_restListenPort;
}

int Configuration::getPromListenPort() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_rest->m_promListenPort;
}

std::string Configuration::getRestListenAddress()
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_rest->m_restListenAddress;
}

std::string Configuration::getDockerProxyAddress() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_rest->m_dockerProxyListenAddr;
}

int Configuration::getRestTcpPort()
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_rest->m_restTcpPort;
}

nlohmann::json Configuration::serializeApplication(bool returnRuntimeInfo, const std::string &user, bool returnUnPersistApp) const
{
	std::lock_guard<std::recursive_mutex> guard(m_appMutex);
	std::vector<std::shared_ptr<Application>> apps;
	std::copy_if(m_apps.begin(), m_apps.end(), std::back_inserter(apps),
				 [this, &user, returnUnPersistApp](std::shared_ptr<Application> app)
				 {
					 return (checkOwnerPermission(user, app->getOwner(), app->getOwnerPermission(), false) &&			 // access permission check
							 ((returnUnPersistApp) || (!returnUnPersistApp && app->isPersistAble())) &&					 // status filter
							 (app->getName() != SEPARATE_REST_APP_NAME) && (app->getName() != SEPARATE_AGENT_APP_NAME)); // not expose rest process
				 });

	auto result = nlohmann::json::array();
	// Build Json
	if (returnRuntimeInfo)
	{
		std::list<os::Process> ptree = os::processes();
		for (std::size_t i = 0; i < apps.size(); ++i)
		{
			result.push_back(apps[i]->AsJson(returnRuntimeInfo, (void *)(&ptree)));
		}
	}
	else
	{
		for (std::size_t i = 0; i < apps.size(); ++i)
		{
			result[i] = apps[i]->AsJson(returnRuntimeInfo);
		}
	}

	return result;
}

void Configuration::deSerializeApps(const nlohmann::json &jsonObj)
{
	for (auto &entry : jsonObj.items())
	{
		// set recover flag used to decrypt confidential data
		auto jsonApp = entry.value();
		jsonApp[JSON_KEY_APP_from_recover] = true;
		auto app = this->parseApp(jsonApp);
		this->addApp2Map(app);
	}
}

void Configuration::disableApp(const std::string &appName)
{
	getApp(appName)->disable();
	saveConfigToDisk();
}
void Configuration::enableApp(const std::string &appName)
{
	auto app = getApp(appName);
	app->enable();
	saveConfigToDisk();
}

const std::string Configuration::getLogLevel() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_logLevel;
}

const std::string Configuration::getDefaultExecUser() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_defaultExecUser;
}

bool Configuration::getDisableExecUser() const
{
	return m_disableExecUser;
}

const std::string Configuration::getWorkDir() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	if (m_defaultWorkDir.length())
		return m_defaultWorkDir;
	else
		return (fs::path(Utility::getParentDir()) / "work").string();
}

bool Configuration::getSslVerifyPeer() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_rest->m_ssl->m_sslVerifyPeer;
}

std::string Configuration::getSSLCertificateFile() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_rest->m_ssl->m_certFile;
}

std::string Configuration::getSSLCertificateKeyFile() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_rest->m_ssl->m_certKeyFile;
}

bool Configuration::getRestEnabled() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_rest->m_restEnabled;
}

std::size_t Configuration::getThreadPoolSize() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_rest->m_httpThreadPoolSize;
}

const std::string Configuration::getDescription() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_hostDescription;
}

const std::shared_ptr<Configuration::JsonConsul> Configuration::getConsul() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_consul;
}

const std::shared_ptr<Configuration::JsonJwt> Configuration::getJwt() const
{
	std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
	return m_rest->m_jwt;
}

bool Configuration::checkOwnerPermission(const std::string &user, const std::shared_ptr<User> &appOwner, int appPermission, bool requestWrite) const
{
	// if app has not defined user, return true
	// if same user, return true
	// if not defined permission, return true
	// if no session user which is internal call, return true
	// if user is admin, return true
	if (user.empty() || appOwner == nullptr || user == appOwner->getName() || appPermission == 0 || user == JWT_ADMIN_NAME)
	{
		return true;
	}

	auto userObj = Security::instance()->getUserInfo(user);
	if (userObj->getGroup() == appOwner->getGroup())
	{
		auto groupPerm = appPermission / 1 % 10;
		if (groupPerm <= static_cast<int>(PERMISSION::GROUP_DENY))
			return false;
		if (!requestWrite &&
			(groupPerm == static_cast<int>(PERMISSION::GROUP_READ) ||
			 groupPerm == static_cast<int>(PERMISSION::GROUP_WRITE)))
		{
			return true;
		}
		if (requestWrite && groupPerm == static_cast<int>(PERMISSION::GROUP_WRITE))
			return true;
	}
	else
	{
		auto otherPerm = 10 * (appPermission / 10 % 10);
		if (otherPerm <= static_cast<int>(PERMISSION::OTHER_DENY))
			return false;
		if (!requestWrite &&
			(otherPerm == static_cast<int>(PERMISSION::OTHER_READ) ||
			 otherPerm == static_cast<int>(PERMISSION::OTHER_WRITE)))
		{
			return true;
		}
		if (requestWrite && otherPerm == static_cast<int>(PERMISSION::OTHER_WRITE))
			return true;
	}
	return false;
}

void Configuration::dump()
{
	const static char fname[] = "Configuration::dump() ";

	LOG_DBG << fname << '\n'
			<< Utility::prettyJson(this->AsJson(false, "").dump());

	auto apps = getApps();
	for (auto &app : apps)
	{
		app->dump();
	}
}

std::shared_ptr<Application> Configuration::addApp(const nlohmann::json &jsonApp, std::shared_ptr<Application> fromApp, bool persistable)
{
	auto app = parseApp(jsonApp);
	bool update = false;
	std::lock_guard<std::recursive_mutex> guard(m_appMutex);
	std::for_each(m_apps.begin(), m_apps.end(), [&app, &update](std::shared_ptr<Application> &mapApp)
				  {
					  if (mapApp->getName() == app->getName())
					  {
						  // Stop existing app and replace
						  mapApp->disable();
						  mapApp = app;
						  update = true;
						  return;
					  } });

	if (!update)
	{
		// Register app
		addApp2Map(app);
	}
	// Write to disk
	{
		if (!persistable)
		{
			app->setUnPersistable();
		}
		if (fromApp)
		{
			app->initMetrics(fromApp);
		}
		else
		{
			app->initMetrics();
		}

		if (app->isPersistAble())
		{
			saveConfigToDisk();
		}
		// invoke immediately
		app->execute();
	}
	app->dump();
	return app;
}

void Configuration::removeApp(const std::string &appName)
{
	const static char fname[] = "Configuration::removeApp() ";

	LOG_DBG << fname << appName;
	std::shared_ptr<Application> app;
	{
		std::lock_guard<std::recursive_mutex> guard(m_appMutex);
		// Update in-memory app
		for (auto iterA = m_apps.begin(); iterA != m_apps.end();)
		{
			if ((*iterA)->getName() == appName)
			{
				app = (*iterA);
				iterA = m_apps.erase(iterA);
				// Write to disk
				if (app->isPersistAble())
				{
					saveConfigToDisk();
				}
				LOG_DBG << fname << "removed " << appName;
			}
			else
			{
				iterA++;
			}
		}
	}
	if (app)
	{
		app->destroy();
	}
}

void Configuration::saveConfigToDisk()
{
	const static char fname[] = "Configuration::saveConfigToDisk() ";

	auto content = (this->AsJson(false, "", false).dump());
	if (content.length())
	{
		std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);
		auto tmpFile = m_jsonFilePath + "." + std::to_string(Utility::getThreadId());
		if (Utility::runningInContainer())
		{
			tmpFile = m_jsonFilePath;
		}
		std::ofstream ofs(tmpFile, ios::trunc);
		if (ofs.is_open())
		{
			auto formatJson = Utility::prettyJson(content);
			ofs << formatJson;
			ofs.close();
			if (tmpFile != m_jsonFilePath)
			{
				if (ACE_OS::rename(tmpFile.c_str(), m_jsonFilePath.c_str()) == 0)
				{
					LOG_DBG << fname << formatJson;
				}
				else
				{
					LOG_ERR << fname << "Failed to write configuration file <" << m_jsonFilePath << ">, error :" << std::strerror(errno);
				}
			}
		}
	}
	else
	{
		LOG_ERR << fname << "Configuration content is empty";
	}
	LOG_DBG << fname;
}

void Configuration::hotUpdate(const nlohmann::json &jsonValue)
{
	const static char fname[] = "Configuration::hotUpdate() ";

	LOG_DBG << fname << "Entered";
	bool consulUpdated = false;
	{
		std::lock_guard<std::recursive_mutex> guard(m_hotupdateMutex);

		// parse
		auto newConfig = Configuration::FromJson((jsonValue.dump()));

		// update
		if (HAS_JSON_FIELD(jsonValue, JSON_KEY_Description))
			SET_COMPARE(this->m_hostDescription, newConfig->m_hostDescription);
		if (HAS_JSON_FIELD(jsonValue, JSON_KEY_LogLevel))
		{
			if (this->m_logLevel != newConfig->m_logLevel)
			{
				Utility::setLogLevel(newConfig->m_logLevel);
				SET_COMPARE(this->m_logLevel, newConfig->m_logLevel);
			}
		}

		if (HAS_JSON_FIELD(jsonValue, JSON_KEY_ScheduleIntervalSeconds))
			SET_COMPARE(this->m_scheduleInterval, newConfig->m_scheduleInterval);
		if (HAS_JSON_FIELD(jsonValue, JSON_KEY_DefaultExecUser))
			SET_COMPARE(this->m_defaultExecUser, newConfig->m_defaultExecUser);
		if (HAS_JSON_FIELD(jsonValue, JSON_KEY_DisableExecUser))
			SET_COMPARE(this->m_disableExecUser, newConfig->m_disableExecUser);
		if (HAS_JSON_FIELD(jsonValue, JSON_KEY_WorkingDirectory))
			SET_COMPARE(this->m_defaultWorkDir, newConfig->m_defaultWorkDir);
		// REST
		if (HAS_JSON_FIELD(jsonValue, JSON_KEY_REST))
		{
			auto rest = jsonValue.at(JSON_KEY_REST);
			if (HAS_JSON_FIELD(rest, JSON_KEY_RestEnabled))
				SET_COMPARE(this->m_rest->m_restEnabled, newConfig->m_rest->m_restEnabled);
			if (HAS_JSON_FIELD(rest, JSON_KEY_RestListenPort))
				SET_COMPARE(this->m_rest->m_restListenPort, newConfig->m_rest->m_restListenPort);
			if (HAS_JSON_FIELD(rest, JSON_KEY_RestTcpPort))
				SET_COMPARE(this->m_rest->m_restTcpPort, newConfig->m_rest->m_restTcpPort);
			if (HAS_JSON_FIELD(rest, JSON_KEY_DockerProxyListenAddr))
				SET_COMPARE(this->m_rest->m_dockerProxyListenAddr, newConfig->m_rest->m_dockerProxyListenAddr);
			if (HAS_JSON_FIELD(rest, JSON_KEY_RestListenAddress))
				SET_COMPARE(this->m_rest->m_restListenAddress, newConfig->m_rest->m_restListenAddress);
			if (HAS_JSON_FIELD(rest, JSON_KEY_HttpThreadPoolSize))
				SET_COMPARE(this->m_rest->m_httpThreadPoolSize, newConfig->m_rest->m_httpThreadPoolSize);
			if (HAS_JSON_FIELD(rest, JSON_KEY_PrometheusExporterListenPort) && (this->m_rest->m_promListenPort != newConfig->m_rest->m_promListenPort))
			{
				SET_COMPARE(this->m_rest->m_promListenPort, newConfig->m_rest->m_promListenPort);
			}
			// SSL
			if (HAS_JSON_FIELD(rest, JSON_KEY_SSL))
			{
				auto ssl = rest.at(JSON_KEY_SSL);
				if (HAS_JSON_FIELD(ssl, JSON_KEY_SSLCertificateFile))
					SET_COMPARE(this->m_rest->m_ssl->m_certFile, newConfig->m_rest->m_ssl->m_certFile);
				if (HAS_JSON_FIELD(ssl, JSON_KEY_SSLCertificateKeyFile))
					SET_COMPARE(this->m_rest->m_ssl->m_certKeyFile, newConfig->m_rest->m_ssl->m_certKeyFile);
				if (HAS_JSON_FIELD(ssl, JSON_KEY_VerifyPeer))
					SET_COMPARE(this->m_rest->m_ssl->m_sslVerifyPeer, newConfig->m_rest->m_ssl->m_sslVerifyPeer);
			}

			// JWT
			if (HAS_JSON_FIELD(rest, JSON_KEY_JWT))
			{
				auto sec = rest.at(JSON_KEY_JWT);
				if (HAS_JSON_FIELD(sec, JSON_KEY_JWTSalt))
					SET_COMPARE(this->m_rest->m_jwt->m_jwtSalt, newConfig->m_rest->m_jwt->m_jwtSalt);
				if (HAS_JSON_FIELD(sec, JSON_KEY_SECURITY_Interface))
					SET_COMPARE(this->m_rest->m_jwt->m_jwtInterface, newConfig->m_rest->m_jwt->m_jwtInterface);
			}
		}

		// Labels
		if (HAS_JSON_FIELD(jsonValue, JSON_KEY_Labels))
			SET_COMPARE(this->m_label, newConfig->m_label);

		// Consul
		if (HAS_JSON_FIELD(jsonValue, JSON_KEY_CONSUL))
		{
			SET_COMPARE(this->m_consul, newConfig->m_consul);
			consulUpdated = true;
		}
	}
	// do not hold Configuration lock to access timer, timer lock is higher level
	if (consulUpdated)
	{
		ConsulConnection::instance()->init();
	}
	ResourceCollection::instance()->getHostName(true);

	this->dump();
	ResourceCollection::instance()->dump();
}

void Configuration::readConfigFromEnv(nlohmann::json &jsonConfig)
{
	const static char fname[] = "Configuration::readConfigFromEnv() ";

	// environment "APPMESH_LogLevel=INFO" can override main configuration
	// environment "APPMESH_Security_JWTEnabled=false" can override Security configuration

	for (char **var = environ; *var != nullptr; var++)
	{
		std::string env = *var;
		auto pos = env.find('=');
		if (Utility::startWith(env, ENV_APPMESH_PREFIX) && (pos != std::string::npos))
		{
			auto envKey = env.substr(0, pos);
			auto envVal = env.substr(pos + 1);
			auto keys = Utility::splitString(envKey, "_");
			nlohmann::json *json = &jsonConfig;
			for (size_t i = 1; i < keys.size(); i++)
			{
				auto jsonKey = keys[i];
				if (json->contains(jsonKey))
				{
					// find the last level
					if (i == (keys.size() - 1))
					{
						// override json value
						if (applyEnvConfig(json->at(jsonKey), envVal))
						{
							LOG_INF << fname << "Configuration: " << envKey << " apply environment value: " << envVal;
						}
						else
						{
							LOG_WAR << fname << "Configuration: " << envKey << " apply environment value: " << envVal << " failed";
						}
					}
					else
					{
						// switch to next level
						json = &(json->at(jsonKey));
					}
				}
			}
		}
	}
}
bool Configuration::applyEnvConfig(nlohmann::json &jsonValue, std::string envValue)
{
	const static char fname[] = "Configuration::applyEnvConfig() ";

	if (jsonValue.is_string())
	{
		jsonValue = std::string(envValue);
		return true;
	}
	else if (jsonValue.is_number())
	{
		jsonValue = (std::stoi(envValue));
		return true;
	}
	else if (jsonValue.is_boolean())
	{
		if (Utility::isNumber(envValue))
		{
			jsonValue = (envValue != "0");
			return true;
		}
		else
		{
			jsonValue = (envValue != "false");
			return true;
		}
	}
	else
	{
		LOG_WAR << fname << "JSON value type not supported: " << jsonValue.dump();
	}
	return false;
}

void Configuration::registerPrometheus()
{
	std::lock_guard<std::recursive_mutex> guard(m_appMutex);
	std::for_each(m_apps.begin(), m_apps.end(), [](std::vector<std::shared_ptr<Application>>::reference p)
				  { p->initMetrics(); });
}

bool Configuration::prometheusEnabled() const
{
	return getRestEnabled() && getPromListenPort() > 1024;
}

std::shared_ptr<Application> Configuration::parseApp(const nlohmann::json &jsonApp)
{
	auto app = std::make_shared<Application>();
	Application::FromJson(app, jsonApp);
	return app;
}

std::shared_ptr<Application> Configuration::getApp(const std::string &appName) const
{
	const static char fname[] = "Configuration::getApp() ";
	std::vector<std::shared_ptr<Application>> apps = getApps();
	auto iter = std::find_if(apps.begin(), apps.end(), [&appName](const std::shared_ptr<Application> &app)
							 { return app->getName() == appName; });
	if (iter != apps.end())
		return *iter;

	LOG_WAR << fname << "No such application: " << appName;
	throw std::invalid_argument("No such application");
}

bool Configuration::isAppExist(const std::string &appName)
{
	std::vector<std::shared_ptr<Application>> apps = getApps();
	return std::any_of(apps.begin(), apps.end(), [&appName](const std::shared_ptr<Application> &app)
					   { return app->getName() == appName; });
}

const nlohmann::json Configuration::getAgentAppJson() const
{
	const static char fname[] = "Configuration::getAgentAppJson() ";

	auto restUri = Utility::stringFormat("https://%s:%d", Configuration::instance()->getRestListenAddress().c_str(), Configuration::instance()->getRestListenPort());
	auto cmd = (fs::path(Utility::getSelfDir()) / "agent").string();
	if (Configuration::instance()->getRestEnabled())
	{
		cmd += std::string(" -rest_tcp_port ") + std::to_string(Configuration::instance()->getRestTcpPort()) +
			   " -agent_url " + Utility::stdStringTrim(restUri, '/');
	}
	if (Configuration::instance()->getDockerProxyAddress().length() &&
		Utility::isFileExist("/var/run/docker.pid") &&
		os::pstree(1)->contains(std::stoi(Utility::readFile("/var/run/docker.pid"))))
	{
		cmd += std::string(" -docker_agent_url ") + this->getDockerProxyAddress();
	}
	else
	{
		LOG_WAR << fname << "docker agent not enabled";
	}
	if (Configuration::instance()->prometheusEnabled())
	{
		cmd += std::string(" -prom_exporter_port ") + std::to_string(Configuration::instance()->getPromListenPort());
	}
	LOG_INF << fname << " agent start command <" << cmd << ">";

	nlohmann::json restApp;
	restApp[JSON_KEY_APP_name] = std::string(SEPARATE_AGENT_APP_NAME);
	restApp[JSON_KEY_APP_command] = std::string(cmd);
	restApp[JSON_KEY_APP_description] = std::string("REST agent for App Mesh");
	restApp[JSON_KEY_APP_owner_permission] = (11);
	restApp[JSON_KEY_APP_owner] = std::string(JWT_ADMIN_NAME);
	restApp[JSON_KEY_APP_stdout_cache_num] = (3);
	auto objBehavior = nlohmann::json::object();
	objBehavior[JSON_KEY_APP_behavior_exit] = std::string(AppBehavior::action2str(AppBehavior::Action::RESTART));
	restApp[JSON_KEY_APP_behavior] = objBehavior;
	return restApp;
}

std::shared_ptr<Configuration::JsonRest> Configuration::JsonRest::FromJson(const nlohmann::json &jsonValue)
{
	const static char fname[] = "Configuration::JsonRest::FromJson() ";

	auto rest = std::make_shared<JsonRest>();
	rest->m_restListenPort = GET_JSON_INT_VALUE(jsonValue, JSON_KEY_RestListenPort);
	rest->m_restListenAddress = GET_JSON_STR_VALUE(jsonValue, JSON_KEY_RestListenAddress);
	rest->m_restTcpPort = GET_JSON_INT_VALUE(jsonValue, JSON_KEY_RestTcpPort);
	rest->m_dockerProxyListenAddr = GET_JSON_STR_VALUE(jsonValue, JSON_KEY_DockerProxyListenAddr);
	rest->m_dockerProxyListenAddr = Utility::stringReplace(rest->m_dockerProxyListenAddr, "https", "http");
	SET_JSON_BOOL_VALUE(jsonValue, JSON_KEY_RestEnabled, rest->m_restEnabled);
	SET_JSON_INT_VALUE(jsonValue, JSON_KEY_PrometheusExporterListenPort, rest->m_promListenPort);
	auto threadpool = GET_JSON_INT_VALUE(jsonValue, JSON_KEY_HttpThreadPoolSize);
	if (threadpool > 0 && threadpool < 40)
	{
		rest->m_httpThreadPoolSize = threadpool;
	}
	if (rest->m_restListenPort < 1000 || rest->m_restListenPort > 65534)
	{
		rest->m_restListenPort = DEFAULT_REST_LISTEN_PORT;
		LOG_INF << fname << "Default value <" << rest->m_restListenPort << "> will by used for RestListenPort";
	}
	if (!Utility::isFileExist("/var/run/docker.sock"))
	{
		LOG_INF << fname << "Docker not installed or started, will not start docker agent.";
		rest->m_dockerProxyListenAddr.clear();
	}
	// SSL
	if (HAS_JSON_FIELD(jsonValue, JSON_KEY_SSL))
	{
		rest->m_ssl = JsonSsl::FromJson(jsonValue.at(JSON_KEY_SSL));
	}
	// JWT
	if (HAS_JSON_FIELD(jsonValue, JSON_KEY_JWT))
	{
		rest->m_jwt = JsonJwt::FromJson(jsonValue.at(JSON_KEY_JWT));
	}
	return rest;
}

nlohmann::json Configuration::JsonRest::AsJson() const
{
	auto result = nlohmann::json::object();
	result[JSON_KEY_RestEnabled] = (m_restEnabled);
	result[JSON_KEY_HttpThreadPoolSize] = ((uint32_t)m_httpThreadPoolSize);
	result[JSON_KEY_RestListenPort] = (m_restListenPort);
	result[JSON_KEY_PrometheusExporterListenPort] = (m_promListenPort);
	result[JSON_KEY_RestListenAddress] = std::string(m_restListenAddress);
	result[JSON_KEY_RestTcpPort] = (m_restTcpPort);
	result[JSON_KEY_DockerProxyListenAddr] = std::string(m_dockerProxyListenAddr);
	// SSL
	result[JSON_KEY_SSL] = m_ssl->AsJson();

	// JWT
	result[JSON_KEY_JWT] = m_jwt->AsJson();
	return result;
}

Configuration::JsonRest::JsonRest()
	: m_restEnabled(false), m_httpThreadPoolSize(DEFAULT_HTTP_THREAD_POOL_SIZE),
	  m_restListenPort(DEFAULT_REST_LISTEN_PORT), m_promListenPort(DEFAULT_PROM_LISTEN_PORT),
	  m_restTcpPort(DEFAULT_TCP_REST_LISTEN_PORT)
{
	m_ssl = std::make_shared<JsonSsl>();
	m_jwt = std::make_shared<JsonJwt>();
}

std::shared_ptr<Configuration::JsonSsl> Configuration::JsonSsl::FromJson(const nlohmann::json &jsonValue)
{
	const static char fname[] = "Configuration::JsonSsl::FromJson() ";
	auto ssl = std::make_shared<JsonSsl>();
	SET_JSON_BOOL_VALUE(jsonValue, JSON_KEY_VerifyPeer, ssl->m_sslVerifyPeer);
	ssl->m_certFile = GET_JSON_STR_VALUE(jsonValue, JSON_KEY_SSLCertificateFile);
	ssl->m_certKeyFile = GET_JSON_STR_VALUE(jsonValue, JSON_KEY_SSLCertificateKeyFile);
	if (!Utility::isFileExist(ssl->m_certFile) && jsonValue.contains(JSON_KEY_SSLCertificateFile))
	{
		LOG_WAR << fname << "SSLCertificateFile not exist: " << ssl->m_certFile;
		throw std::invalid_argument("SSLCertificateFile not exist");
	}
	if (!Utility::isFileExist(ssl->m_certKeyFile) && jsonValue.contains(JSON_KEY_SSLCertificateKeyFile))
	{
		LOG_WAR << fname << "SSLCertificateKeyFile not exist: " << ssl->m_certKeyFile;
		throw std::invalid_argument("SSLCertificateKeyFile not exist");
	}
	return ssl;
}

nlohmann::json Configuration::JsonSsl::AsJson() const
{
	auto result = nlohmann::json::object();
	result[JSON_KEY_VerifyPeer] = (m_sslVerifyPeer);
	result[JSON_KEY_SSLCertificateFile] = std::string(m_certFile);
	result[JSON_KEY_SSLCertificateKeyFile] = std::string(m_certKeyFile);
	return result;
}

Configuration::JsonSsl::JsonSsl()
	: m_sslVerifyPeer(false)
{
}

Configuration::JsonJwt::JsonJwt()
{
}

std::shared_ptr<Configuration::JsonJwt> Configuration::JsonJwt::FromJson(const nlohmann::json &jsonObj)
{
	auto security = std::make_shared<Configuration::JsonJwt>();
	security->m_jwtSalt = GET_JSON_STR_VALUE(jsonObj, JSON_KEY_JWTSalt);
	security->m_jwtInterface = GET_JSON_STR_VALUE(jsonObj, JSON_KEY_SECURITY_Interface);
	return security;
}

nlohmann::json Configuration::JsonJwt::AsJson() const
{
	auto result = nlohmann::json::object();
	result[JSON_KEY_JWTSalt] = std::string(m_jwtSalt);
	result[JSON_KEY_SECURITY_Interface] = std::string(m_jwtInterface);
	return result;
}

std::string Configuration::JsonJwt::getJwtInterface() const
{
	return m_jwtInterface;
}

std::shared_ptr<Configuration::JsonConsul> Configuration::JsonConsul::FromJson(const nlohmann::json &jsonObj, int appmeshRestPort)
{
	const static char fname[] = "Configuration::JsonConsul::FromJson() ";
	auto consul = std::make_shared<JsonConsul>();
	consul->m_consulUrl = GET_JSON_STR_VALUE(jsonObj, JSON_KEY_CONSUL_URL);
	consul->m_proxyUrl = GET_JSON_STR_VALUE(jsonObj, JSON_KEY_CONSUL_APPMESH_PROXY_URL);
	consul->m_basicAuthPass = GET_JSON_STR_VALUE(jsonObj, JSON_KEY_CONSUL_AUTH_PASS);
	consul->m_basicAuthUser = GET_JSON_STR_VALUE(jsonObj, JSON_KEY_CONSUL_AUTH_USER);
	consul->m_isMaster = GET_JSON_BOOL_VALUE(jsonObj, JSON_KEY_CONSUL_IS_MAIN);
	consul->m_isWorker = GET_JSON_BOOL_VALUE(jsonObj, JSON_KEY_CONSUL_IS_WORKER);
	SET_JSON_INT_VALUE(jsonObj, JSON_KEY_CONSUL_SESSION_TTL, consul->m_ttl);
	SET_JSON_BOOL_VALUE(jsonObj, JSON_KEY_CONSUL_SECURITY, consul->m_securitySync);
	const static boost::regex urlExpr("(http|https)://((\\w+\\.)*\\w+)(\\:[0-9]+)?");
	if (consul->m_consulUrl.length() && !boost::regex_match(consul->m_consulUrl, urlExpr))
	{
		LOG_WAR << fname << "incorrect Consul url: " << consul->m_consulUrl;
		throw std::invalid_argument("incorrect Consul url");
	}
	if (consul->m_ttl < 5 && jsonObj.contains(JSON_KEY_CONSUL_SESSION_TTL))
		throw std::invalid_argument("session TTL should not less than 5s");

	{
		auto hostname = ResourceCollection::instance()->getHostName();
		consul->m_defaultProxyUrl = Utility::stringFormat("https://%s:%d", hostname.c_str(), appmeshRestPort);
	}
	return consul;
}

nlohmann::json Configuration::JsonConsul::AsJson() const
{
	auto result = nlohmann::json::object();
	if (m_consulUrl.length())
		result[JSON_KEY_CONSUL_URL] = std::string(m_consulUrl);
	result[JSON_KEY_CONSUL_IS_MAIN] = (m_isMaster);
	result[JSON_KEY_CONSUL_IS_WORKER] = (m_isWorker);
	result[JSON_KEY_CONSUL_SESSION_TTL] = (m_ttl);
	result[JSON_KEY_CONSUL_SECURITY] = (m_securitySync);
	if (m_proxyUrl.length())
		result[JSON_KEY_CONSUL_APPMESH_PROXY_URL] = std::string(m_proxyUrl);
	if (m_basicAuthUser.length())
		result[JSON_KEY_CONSUL_AUTH_USER] = std::string(m_basicAuthUser);
	if (m_basicAuthPass.length())
		result[JSON_KEY_CONSUL_AUTH_PASS] = std::string(m_basicAuthPass);
	return result;
}

bool Configuration::JsonConsul::consulEnabled() const
{
	return !m_consulUrl.empty();
}

bool Configuration::JsonConsul::consulSecurityEnabled() const
{
	return !m_consulUrl.empty() && m_securitySync;
}

const std::string Configuration::JsonConsul::appmeshUrl() const
{
	if (m_proxyUrl.empty())
		return m_defaultProxyUrl;
	else
		return m_proxyUrl;
}

Configuration::JsonConsul::JsonConsul()
	: m_isMaster(false), m_isWorker(false), m_ttl(CONSUL_SESSION_DEFAULT_TTL), m_securitySync(false)
{
}
