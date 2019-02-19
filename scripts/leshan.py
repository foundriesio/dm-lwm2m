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
import sys

# Script Version 1.1

headers = { 'Content-Type': 'application/json'}
thread_wait = 5

logging.basicConfig(level=logging.INFO,
                    format='[%(levelname)s] (%(threadName)-10s) %(message)s',
                    )

class UpdateAction:
    # url must always be set by caller
    def __init__(self, client=None, url='url',
                 hostname='http://mgmt.foundries.io/leshan', monitor=False):
        self.client = client;
        self.url = url;
        self.hostname = hostname;
        self.monitor = monitor;
        self.download_status = 0;
        self.update_result = 0;
        self.time_start = datetime.datetime.now();
        self.time_end = datetime.datetime.now();
        self.result = False;
        self.requested = False;
        self.abort_thread = False;

update_list = []
aborted = False

def signal_handler(signal, frame):
    global aborted

    print('Script aborting ...')
    aborted = True
    for ua in update_list:
        ua.abort_thread = True

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
    if response.status_code in (200, 201):
        return True
    else:
        logging.error(response)
        return False

def get(url, raw=False):
    response = requests.get(url, headers=headers)
    if response.status_code in (200, 201):
        try:
            payload = response.json()
        except:
            return -1
        if raw:
            return payload
        else:
            if 'content' in payload:
                if 'value' in payload['content']:
                    return payload['content']['value']
    else:
        logging.error(response)
        return -1

def put(url, data):
    response = requests.put(url, json=data, headers=headers)
    if response.status_code in (200, 201):
        return True
    else:
        logging.error(response)
        return False

def update(ua, thread_count):
    ua.time_start = datetime.datetime.now()
    download_status_url = '%s/api/clients/%s/5/0/3' % (ua.hostname, ua.client)
    update_result_url = '%s/api/clients/%s/5/0/5' % (ua.hostname, ua.client)
    while ua.abort_thread == False:
        ua.download_status = get(download_status_url)
        if ua.download_status == 0:
            if ua.requested == False:
                logging.info('ready for firmware update')
                firmware = {'id': 1, 'value': ua.url}
                firmware_url = '%s/api/clients/%s/5/0/1' % (ua.hostname, ua.client)
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
                ua.update_result = get(update_result_url)
                logging.error('failed to start firmware download (%d)', ua.update_result)
                ua.result = False
                break
        if ua.download_status == 1:
            logging.info('downloading firmware')
        if ua.download_status == 2:
            logging.info('ready to apply update')
            exec_update_url = '%s/api/clients/%s/5/0/2' % (ua.hostname, ua.client)
            if (post(exec_update_url)):
                logging.info('requested firmware update execution')
                check = True
                while (not ua.abort_thread and check):
                    ua.update_result = get(update_result_url)
                    if ua.update_result == 1:
                        logging.info('firmware update successful')
                        check = False
                    elif ua.update_result > 1:
                        logging.error('firmware update failed (%d)', ua.update_result)
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
        if ua.download_status < 0:
            logging.error('no longer found')
        # TODO check timeout?
        time.sleep(thread_wait)
    ua.time_end = datetime.datetime.now()
    thread_count.dec()

def run(client, url, hostname, monitor, device, max_threads):
    global aborted

    start_time = datetime.datetime.now()
    thread_count = AtomicCounter()
    client_list_url = '%s/api/clients'  % (hostname)
    response = get(client_list_url, raw=True)
    if response:
        for target in response:
            perform_update = True
            if 'endpoint' in target:
                if client:
                    # check for a partial match of the endpoint
                    if not client in target['endpoint']:
                        perform_update = False
                if perform_update and device:
                    endpoint_url = '%s/api/clients/%s/3/0/1'  % (hostname, target['endpoint'])
                    endpoint_device = get(endpoint_url)
                    if (endpoint_device != device):
                        perform_update = False
                if perform_update:
                    # check for max threads and wait if needed
                    while (not aborted and thread_count.value >= max_threads):
                        time.sleep(thread_wait)
                    if (not aborted):
                        # bump thread count
                        thread_count.inc()
                        # append a new update action
                        ua = UpdateAction(target['endpoint'], url, hostname, monitor)
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
        if ua.abort_thread == True:
            logging.info('[%s] update ABORTED (%d seconds)', ua.client, timediff.seconds)
        elif ua.result:
            logging.info('[%s] update SUCCESS (%d seconds)', ua.client, timediff.seconds)
        else:
            logging.info('[%s] update FAILED(%d) (%d seconds)', ua.client, ua.update_result, timediff.seconds)
            result = 1
    timediff = datetime.datetime.now() - start_time
    logging.info('%d update(s) attempted which took %d seconds total', count, timediff.seconds)
    sys.exit(result)

def main():
    # add process interrupt handler
    signal.signal(signal.SIGINT, signal_handler)

    description = 'Simple Leshan API wrapper for firmware updates'
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('-c', '--client', help='Leshan Client ID, if not specified all targets will be updated', default=None)
    parser.add_argument('-u', '--url', help='URL for client firmware (http:// or coap://)', required=True)
    parser.add_argument('-host', '--hostname', help='Leshan server URL', default='https://mgmt.foundries.io/leshan')
    parser.add_argument('-m', '--monitor', help='Monitor the update', action='store_true', default=False)
    parser.add_argument('-d', '--device', help='Device type filter', default=None)
    parser.add_argument('-t', '--threads', help='Maximum threads', default=1)
    args = parser.parse_args()
    run(args.client, args.url, args.hostname, args.monitor, args.device, int(args.threads))

if __name__ == '__main__':
    main()
