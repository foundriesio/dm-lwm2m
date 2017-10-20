import argparse
import requests
import json
import time

__version__ = 1.0

headers = { 'Content-Type': 'application/json'}

def post(url):
    response = requests.post(url, headers=headers)
    if response.status_code == 200 or 201:
        return True
    else:
        print response
        return False

def get(url):
    response = requests.get(url, headers=headers)
    if response.status_code == 200 or 201:
        try:
            payload = json.loads(response.content)
	    return payload
        except:
            return None
    else:
        print response
        return None

def put(url, data):
    response = requests.put(url, data=json.dumps(data), headers=headers)
    if response.status_code == 200 or 201:
        return True
    else:
        print response
        return False

def run(targets, hostname, port):
    run = True
    while run:
        client_list_url = 'http://%s:%s/api/clients'  % (hostname, port)
        response = get(client_list_url)
        if response:
            if len(response) >= targets:
                print "Matched number of targets"
                run = False
            else:
                print "Only %s target matches, sleeping..." % len(response)
                time.sleep(5)

def main():
    description = 'Simple Leshan API Wrapper for waiting for targets to connect'
    parser = argparse.ArgumentParser(version=__version__, description=description)
    parser.add_argument('-t', '--targets', help='Number of Leshan Targets to wait for', type=int, required=True)
    parser.add_argument('-host', '--hostname', help='Leshan Server Hostname or IP', default='leshan')
    parser.add_argument('-port', '--port', help='Leshan Server Port', default='8080')
    args = parser.parse_args()
    run(args.targets, args.hostname, args.port)

if __name__ == '__main__':
    main()
