#!/usr/local/bin/python
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
thread_wait = .5

logging.basicConfig(level=logging.INFO,
                    format='[%(levelname)s] (%(threadName)-10s) %(message)s',
                    )

class ToggleAction:
    # url must always be set by caller
    def __init__(self, client=None,
                 hostname='mgmt.foundries.io', port=8080):
        self.client = client;
        self.hostname = hostname;
        self.port = port;
        self.light_on_off = False;
        self.time_start = 0;
        self.time_end = 0;
        self.result = False;
        self.requested = False;
        self.abort_thread = False;

toggle_list = []
aborted = False

def signal_handler(signal, frame):
    print('Script aborting ...')
    aborted = True
    for ua in toggle_list:
        ua.abort_thread = True

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
    #logging.info(url)
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

def toggle(ua, thread_count=15):
    ua.time_start = datetime.datetime.now()
    #while ua.abort_thread == False:
    if 1 == 1:
        light_onoff_url = 'http://%s:%s/api/clients/%s/3311/0/5850' % (ua.hostname, ua.port, ua.client)

        ua.light_on_off = get(light_onoff_url)
        if ua.light_on_off == True:
            logging.info('light is on')
            nextstate = {'id': 5850, 'value': False}
            ua.light_on_off = put(light_onoff_url, nextstate)
        else:
            logging.info('light is off')
            nextstate = {'id': 5850, 'value': True}
            ua.light_on_off = put(light_onoff_url, nextstate)
    ua.time_end = datetime.datetime.now()
    thread_count.dec()

def run(client, hostname, port, device, max_threads):
    start_time = datetime.datetime.now()
    thread_count = AtomicCounter()
    if client:
        # bump thread count
        thread_count.inc()
        # append a new update action
        ua = UpdateAction(client, hostname, port)
        light(ua, thread_count)
        if ua.result:
            logging.info('%s run completed', client)
            exit(0)
        else:
            logging.error('%s failed to run, aborting...', client)
            exit(1)
    else:
        client_list_url = 'http://%s:%s/api/clients'  % (hostname, port)
        response = get(client_list_url, raw=True)
        if response:
            for target in response:
                toggle_light = True
                if 'endpoint' in target:
                    if device:
                        endpoint_url = 'http://%s:%s/api/clients/%s/3/0/1'  % (hostname, port, target['endpoint'])
                        endpoint_device = get(endpoint_url)
                        if (endpoint_device != device):
                            toggle_light = False
                    if toggle_light:
                        # check for max threads and wait if needed
                        while (thread_count.value >= max_threads):
                            time.sleep(thread_wait)
                        # bump thread count
                        thread_count.inc()
                        # append a new update action
                        ua = ToggleAction(target['endpoint'], hostname, port)
                        toggle_list.append(ua)

                        # create a new thread
                        t = threading.Thread(name=target['endpoint'], target=toggle,
                                             args=(ua, thread_count,))
                        t.start()
            while (aborted == False and thread_count.value > 0):
                # TODO check timeout?
                time.sleep(thread_wait)
    # dump update info
    result = 0
    if 1 == 0:
        logging.info('SUMMARY:')
        count = 0
        for ua in toggle_list:
            count += 1
            timediff = ua.time_end - ua.time_start
            if ua.abort_thread == True:
                logging.info('[%s] ABORTED (%d seconds)', ua.client, timediff.seconds)
            elif ua.result:
                logging.info('[%s] SUCCESS (%d seconds)', ua.client, timediff.seconds)
            else:
                logging.info('[%s] FAILED (%d seconds)', ua.client, timediff.seconds)
                result = 1
            timediff = datetime.datetime.now() - start_time
            logging.info('%d toggles attempted which took %d seconds total', count, timediff.seconds)
    exit(result)

def main():
    description = 'Simple Leshan API wrapper for light toggle'
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('-c', '--client', help='Leshan Client ID, if not specified all targets will be updated', default=None)
    parser.add_argument('-s', '--hostname', help='Leshan server hostname or ip', default='mgmt.foundries.io')
    parser.add_argument('-p', '--port', help='Leshan server port', default='8080')
    parser.add_argument('-d', '--device', help='Device type filter', default=None)
    parser.add_argument('-t', '--threads', help='Maximum threads', default=1)
    args = parser.parse_args()
    logging.info('client:%s hostname:%s port:%s device:%s threads:%d', args.client, args.hostname, args.port, args.device, int(args.threads))
    run(args.client, args.hostname, args.port, args.device, int(args.threads))

if __name__ == '__main__':
    main()
