#ifndef MOUS_IPLUGINMANAGER_H
#define MOUS_IPLUGINMANAGER_H

#include <vector>
#include <string>
#include <util/ErrorCode.h>
#include <util/PluginDef.h>

namespace mous {

struct PluginInfo;
class IPluginAgent;

class IPluginManager
{
public:
    static IPluginManager* Create();
    static void Free(IPluginManager*);

public:
    virtual ~IPluginManager() { }

    virtual size_t LoadPluginDir(const std::string& dir) = 0;
    virtual EmErrorCode LoadPlugin(const std::string& path) = 0;
    virtual void UnloadPlugin(const std::string& path) = 0;
    virtual void UnloadAll() = 0;

    virtual void Plugins(std::vector<const IPluginAgent*>& list, EmPluginType) const = 0;
    virtual void PluginPath(std::vector<std::string>& list) const = 0;
    virtual const PluginInfo* QueryPluginInfo(const std::string& path) const = 0;
};

}

#endif
