#!/usr/bin/python3
import json
import sys

# import App Mesh SDK from installation path
sys.path.append("/opt/appmesh/sdk/")
import appmesh_client

def main():
    # Create a App Mesh Client object
    client = appmesh_client.AppMeshClient()
    # Authenticate
    token = client.login("admin", "admin123")
    if token:
        # call SDK to view application 'ping'
        print(json.dumps(client.app_view("ping"), indent=2))

if __name__ == "__main__":
    main()
