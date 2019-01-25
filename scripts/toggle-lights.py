#!/usr/local/bin/python
# Copyright (c) 2018-2019 Foundries.io
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
thread_wait = .25

logging.basicConfig(level=logging.INFO,
                    format='[%(levelname)s] (%(threadName)-10s) %(message)s',
                    )

class ToggleAction:
    # url must always be set by caller
    def __init__(self, client=None,
                 hostname='https://mgmt.foundries.io/leshan'):
        self.client = client;
        self.hostname = hostname;
        self.light_on_off = False;
        self.result = False;
        self.requested = False;
        self.abort_thread = False;

toggle_list = []
aborted = False

def signal_handler(signal, frame):
    global aborted

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
    if response.status_code in (200, 201):
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
    if response.status_code in (200, 201):
        return True
    else:
        logging.error(response)
        return False

def toggle(ua, thread_count):
    global light_on_off

    light_onoff_url = '%s/api/clients/%s/3311/0/5850' % (ua.hostname, ua.client)
    ua.light_on_off = not get(light_onoff_url)
    logging.info('light is %s', 'on' if ua.light_on_off else 'off')
    nextstate = {'id': 5850, 'value': ua.light_on_off}
    ua.result = put(light_onoff_url, nextstate)
    thread_count.dec()

def run(client, hostname, device, max_threads, num_loops, loop_delay):
    global aborted

    thread_count = AtomicCounter()
    loop_counter = num_loops
    while aborted == False and loop_counter >= 0:
        # if num_loops is 0 then run endlessly
        if num_loops > 0:
            loop_counter -= 1

        if client:
            # bump thread count
            thread_count.inc()
            # append a new update action
            ua = ToggleAction(client, hostname)
            toggle(ua, thread_count)
            if ua.result:
                logging.info('%s run completed', client)
                exit(0)
            else:
                logging.error('%s failed to run, aborting...', client)
                exit(1)
        else:
            client_list_url = '%s/api/clients'  % (hostname)
            response = get(client_list_url, raw=True)
            if response:
                for target in response:
                    toggle_light = True
                    if 'endpoint' in target:
                        if device:
                            endpoint_url = '%s/api/clients/%s/3/0/1'  % (hostname, target['endpoint'])
                            endpoint_device = get(endpoint_url)
                            if (endpoint_device != device):
                                toggle_light = False
                        if toggle_light:
                            # check for max threads and wait if needed
                            while (aborted == False and thread_count.value >= max_threads):
                                time.sleep(thread_wait)

                            if (aborted == False):
                                # bump thread count
                                thread_count.inc()
                                # append a new update action
                                ua = ToggleAction(target['endpoint'], hostname)
                                toggle_list.append(ua)

                                # create a new thread
                                t = threading.Thread(name=target['endpoint'], target=toggle,
                                                     args=(ua, thread_count,))
                                t.start()
                while (aborted == False and thread_count.value > 0):
                    # TODO check timeout?
                    time.sleep(thread_wait)

            time.sleep(loop_delay)

    exit(0)

def main():
    description = 'Simple Leshan API wrapper for light toggle'
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('-c', '--client', help='Leshan Client ID, if not specified all targets will be updated', default=None)
    parser.add_argument('-host', '--hostname', help='Leshan server URL', default='https://mgmt.foundries.io/leshan')
    parser.add_argument('-d', '--device', help='Device type filter', default=None)
    parser.add_argument('-t', '--threads', help='Maximum threads', default=1)
    parser.add_argument('-l', '--loops', help='Number of loop executions', default=0)
    parser.add_argument('-w', '--wait', help='Wait delay between loops (in seconds)', default=1)
    args = parser.parse_args()
    logging.info('client:%s hostname:%s device:%s threads:%d loops:%d delay:%d',
        args.client, args.hostname, args.device, int(args.threads), int(args.loops), int(args.wait))
    run(args.client, args.hostname, args.device, int(args.threads), int(args.loops), int(args.wait))

if __name__ == '__main__':
    main()
