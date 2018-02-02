# Copyright (c) 2018 Foundries.io
#
# SPDX-License-Identifier: Apache-2.0

import signal
import argparse
import requests
import json
import time
import logging
import threading
import datetime

# Script Version 1.1

headers = { 'Content-Type': 'application/json'}
thread_wait = 5

logging.basicConfig(level=logging.INFO,
                    format='[%(levelname)s] (%(threadName)-10s) %(message)s',
                    )

class UpdateAction:
    # url must always be set by caller
    def __init__(self, client=None, url='url',
                 hostname='mgmt.foundries.io', port=8080, monitor=False):
        self.client = client;
        self.url = url;
        self.hostname = hostname;
        self.port = port;
        self.monitor = monitor;
        self.download_status = 0;
        self.update_result = 0;
        self.time_start = 0;
        self.time_end = 0;
        self.result = False;
        self.requested = False;

update_list = []

def signal_handler(signal, frame):
    print('Script aborted')
    exit(0)

# add process interrupt handler
signal.signal(signal.SIGINT, signal_handler)

class AtomicCounter:
    def __init__(self, initial=0):
        self.value = initial
        self._lock = threading.Lock()

    def inc(self, num=1):
        self._lock.acquire()
        self.value += num
        self._lock.release()

    def dec(self, num=1):
        self._lock.acquire()
        self.value -= num
        self._lock.release()

def post(url):
    response = requests.post(url, headers=headers)
    if response.status_code == 200 or 201:
        return True
    else:
        logging.error(response)
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
        logging.error(response)
        return None

def put(url, data):
    response = requests.put(url, data=json.dumps(data), headers=headers)
    if response.status_code == 200 or 201:
        return True
    else:
        logging.error(response)
        return False

def update(ua, thread_count):
    ua.time_start = datetime.datetime.now()
    while True:
        download_status_url = 'http://%s:%s/api/clients/%s/5/0/3' % (ua.hostname, ua.port, ua.client)
        ua.download_status = get(download_status_url)
        if ua.download_status == 0:
            if ua.requested == False:
                logging.info('ready for firmware update')
                firmware = {'id': 1, 'value': ua.url}
                firmware_url = 'http://%s:%s/api/clients/%s/5/0/1' % (ua.hostname, ua.port, ua.client)
                if (put(firmware_url, firmware)):
                    logging.info('requested firmware download from %s', ua.url)
                else:
                    logging.error('failed to request firmware download')
                    ua.result = False
                    break
                ua.requested = True
                if not ua.monitor:
                    logging.info('not monitoring device -- end of update')
                    ua.result = True
                    break
            else:
                update_result_url = 'http://%s:%s/api/clients/%s/5/0/5' % (ua.hostname, ua.port, ua.client)
                ua.update_result = get(update_result_url)
                logging.error('failed to start firmware download (%d)', ua.update_result)
                ua.result = False
                break
        if ua.download_status == 1:
            logging.info('downloading firmware')
        if ua.download_status == 2:
            logging.info('ready to apply update')
            exec_update_url = 'http://%s:%s/api/clients/%s/5/0/2' % (ua.hostname, ua.port, ua.client)
            if (post(exec_update_url)):
                logging.info('requested firmware update execution')
                check = True
                update_status_url = 'http://%s:%s/api/clients/%s/5/0/5' % (ua.hostname, ua.port, ua.client)
                while check:
                    if get(update_status_url) == 1:
                        logging.info('firmware update successful')
                        check = False
                    else:
                        # TODO check timeout / bad update status
                        time.sleep(thread_wait)
            else:
                logging.error('failed to request firmware update execution')
                ua.result = False
                break
            logging.info('update completed')
            ua.result = True
            break
        if ua.download_status == 3:
            logging.info('executing the firmware update')
        if ua.download_status == 4:
            logging.error('unknown status')
        if ua.download_status is None:
            logging.error('no longer found')
        # TODO check timeout?
        time.sleep(thread_wait)
    ua.time_end = datetime.datetime.now()
    thread_count.dec()

def run(client, url, hostname, port, monitor, device, max_threads):
    start_time = datetime.datetime.now()
    thread_count = AtomicCounter()
    if client:
        # bump thread count
        thread_count.inc()
        # append a new update action
        ua = UpdateAction(client, url, hostname, port, monitor)
        update(ua, thread_count)
        if ua.result:
            logging.info('%s update completed', client)
            exit(0)
        else:
            logging.error('%s failed to udpate, aborting...', client)
            exit(1)
    else:
        client_list_url = 'http://%s:%s/api/clients'  % (hostname, port)
        response = get(client_list_url, raw=True)
        if response:
            for target in response:
                perform_update = True
                if 'endpoint' in target:
                    if device:
                        endpoint_url = 'http://%s:%s/api/clients/%s/3/0/1'  % (hostname, port, target['endpoint'])
                        endpoint_device = get(endpoint_url)
                        if (endpoint_device != device):
                            perform_update = False
                    if perform_update:
                        # check for max threads and wait if needed
                        while (thread_count.value >= max_threads):
                            time.sleep(thread_wait)
                        # bump thread count
                        thread_count.inc()
                        # append a new update action
                        ua = UpdateAction(target['endpoint'], url, hostname, port, monitor)
                        update_list.append(ua)
			# create a new thread
                        t = threading.Thread(name=target['endpoint'], target=update,
                                             args=(ua, thread_count,))
                        t.start()
            while (thread_count.value > 0):
                # TODO check timeout?
                time.sleep(thread_wait)
    # dump update info
    logging.info('UPDATE SUMMARY:')
    result = 0
    count = 0
    for ua in update_list:
        count += 1
        timediff = ua.time_end - ua.time_start
        if ua.result:
            logging.info('[%s] update SUCCESS (%d seconds)', ua.client, timediff.seconds)
        else:
            logging.info('[%s] update FAILED (%d seconds)', ua.client, timediff.seconds)
            result = 1
    timediff = datetime.datetime.now() - start_time
    logging.info('%d update(s) attempted which took %d seconds total', count, timediff.seconds)
    exit(result)

def main():
    description = 'Simple Leshan API wrapper for firmware updates'
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('-c', '--client', help='Leshan Client ID, if not specified all targets will be updated', default=None)
    parser.add_argument('-u', '--url', help='URL for client firmware (http:// or coap://)', required=True)
    parser.add_argument('-host', '--hostname', help='Leshan server hostname or ip', default='mgmt.foundries.io')
    parser.add_argument('-port', '--port', help='Leshan server port', default='8080')
    parser.add_argument('-m', '--monitor', help='Monitor the update', action='store_true', default=False)
    parser.add_argument('-d', '--device', help='Device type filter', default=None)
    parser.add_argument('-t', '--threads', help='Maximum threads', default=1)
    args = parser.parse_args()
    run(args.client, args.url, args.hostname, args.port, args.monitor, args.device, int(args.threads))

if __name__ == '__main__':
    main()
