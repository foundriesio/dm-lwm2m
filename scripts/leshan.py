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

def get(url, raw=False):
    response = requests.get(url, headers=headers)
    if response.status_code == 200 or 201:
        try:
            payload = json.loads(response.content)
        except:
            return None
        if raw:
            return payload
        else:
            if 'content' in payload:
                if 'value' in payload['content']:
                    return payload['content']['value']
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

def update(client, url, hostname, port, monitor):
    run = True
    while run:
        download_status_url = 'http://%s:%s/api/clients/%s/5/0/3'  % (hostname, port, client)
        dl_status = get(download_status_url)
        if dl_status == 0:
            print "%s is ready for firmware update" % client
            firmware = {'id': 1, 'value': url}
            firmware_url = 'http://%s:%s/api/clients/%s/5/0/1' % (hostname, port, client)
            if (put(firmware_url, firmware)):
                print "requested firmware download for %s" % client
            else:
                print "failed to request firmware download for %s" % client
                run = False
                return run
            if not monitor:
                run = False
        if dl_status == 1:
            print "%s is downloading firmware update from %s" % (client, url)
        if dl_status == 2:
            print "%s is ready for update to apply update" % client
            exec_update_url = 'http://%s:%s/api/clients/%s/5/0/2' % (hostname, port, client)
            if (post(exec_update_url)):
                print "requested firmware update execution for %s" % client
                check = True
                update_status_url = 'http://%s:%s/api/clients/%s/5/0/5'  % (hostname, port, client)
                while check:
                    if get(update_status_url) == 1:
                        print "firmware update for %s successful" % client
                        check = False
                    else:
                        time.sleep(5)
            else:
                print "failed to request firmware update execution for %s" % client
                run = False
                return run
            run = False
            return True
        if dl_status == 3:
            print "%s is executing the firmware update" % client
        if dl_status == 4:
            print "unknown status"
        if dl_status is None:
            print "%s is no longer found" % client
        time.sleep(5)

def run(client, url, hostname, port, monitor):
    if client:
        ret = update(client, url, hostname, port, monitor)
        if ret:
            print "%s update completed" % client
            exit(0)
        else:
            print "%s failed to udpate, aborting..." % client
            exit(1)
    else:
        client_list_url = 'http://%s:%s/api/clients'  % (hostname, port)
        response = get(client_list_url, raw=True)
        if response:
            for target in response:
                if 'endpoint' in target:
                    ret = update(target['endpoint'], url, hostname, port, monitor)
                    if ret:
                         print "%s update completed" % target['endpoint']
                    else:
                        print "%s failed to update, aborting..." % target['endpoint']
                        exit(1)
            exit(0)

def main():
    description = 'Simple Leshan API wrapper for firmware updates'
    parser = argparse.ArgumentParser(version=__version__, description=description)
    parser.add_argument('-c', '--client', help='Leshan Client ID, if not specified all targets will be updated', default=None)
    parser.add_argument('-u', '--url', help='URL for client firmware (http:// or coap://)', required=True)
    parser.add_argument('-host', '--hostname', help='Leshan server hostname or ip', default='leshan')
    parser.add_argument('-port', '--port', help='Leshan server port', default='8080')
    parser.add_argument('-m', '--monitor', help='Monitor the update', action='store_true', default=False)
    args = parser.parse_args()
    run(args.client, args.url, args.hostname, args.port, args.monitor)

if __name__ == '__main__':
    main()
