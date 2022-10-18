#!/usr/bin/python3
import json
import sys
import os
import inspect

# For installation env:
sys.path.append("/opt/appmesh/sdk/")
import appmesh_client

client = appmesh_client.AppMeshClientTCP()
# authentication
token = client.login("admin", "admin123")
print(client.authentication(token, "app-view"))
print(client.user_passwd_update("admin123"))
print(json.dumps(client.permissions_view(), indent=2))
print(
    client.role_update(
        "manage", ["app-control", "app-delete", "cloud-app-reg", "cloud-app-delete", "app-reg", "config-set", "file-download", "file-upload", "label-delete", "label-set"]
    )
)
print(json.dumps(client.permissions_for_user(), indent=2))
print(json.dumps(client.groups_view(), indent=2))
print(json.dumps(client.roles_view(), indent=2))
print(json.dumps(client.user_self(), indent=2))
print(json.dumps(client.users_view(), indent=2))

# view application
print(json.dumps(client.app_view("ping"), indent=2))
print(json.dumps(client.app_view("ping2"), indent=2))
print(json.dumps(client.app_view_all(), indent=2))
print(client.app_output("docker"))
print(client.app_health("ping"))
# manage application
print(json.dumps(client.app_add({"command": "ping www.baidu.com -w 5", "name": "SDK"}), indent=2))
print(client.app_delete("SDK"))
print(client.app_disable("ping"))
print(client.app_enable("ping"))

print(json.dumps(client.host_resource(), indent=2))

print(client.tag_add("MyTag", "TagValue"))
print(client.tag_view())
print(client.app_output("docker"))
print(client.app_health("docker"))
print(client.metrics())
# config
print(client.config_view())
print(json.dumps(client.config_set({"REST": {"SSL": {"SSLEnabled": True}}}), indent=2))
print(client.log_level_set("DEBUG"))
# file
print(client.file_download("/opt/appmesh/log/server.log", "1.log"))
print(client.file_upload(local_file="1.log", file_path="/tmp/2.log"))
# cloud
print(json.dumps(client.cloud_app_view_all(), indent=2))
print(
    client.cloud_app_add(
        {
            "condition": {"arch": "x86_64", "os_version": "centos7.6"},
            "content": {
                "command": "sleep 30",
                "metadata": "cloud-sdk-app",
                "name": "cloud",
                "shell": True,
            },
            "port": 6667,
            "priority": 0,
            "replication": 1,
            "memoryMB": 1024,
        }
    )
)
print(json.dumps(client.cloud_app_delete("cloud"), indent=2))
print(json.dumps(client.cloud_nodes(), indent=2))
# run app
print(client.run_sync({"command": "ping www.baidu.com -w 5", "shell": True}, max_time_seconds=3))
tuple = client.run_async({"command": "ping www.baidu.com -w 5", "shell": True}, max_time_seconds=6)
client.run_async_wait(tuple)
