# Copyright (c) 2018 Foundries.io
#
# SPDX-License-Identifier: Apache-2.0

import argparse
import requests
import json
import time
import signal
import sys

__version__ = 1.0

headers = { 'Content-Type': 'application/json'}

aborted = False

def signal_handler(signal, frame):
    global aborted

    print('Script aborting ...')
    aborted = True

def post(url):
    response = requests.post(url, headers=headers)
    if response.status_code in (200, 201):
        return True
    else:
        print(response)
        return False

def get(url):
    response = requests.get(url, headers=headers)
    if response.status_code in (200, 201):
        try:
            return response.json()
        except:
            return None
    else:
        print(response)
        return None

def put(url, data):
    response = requests.put(url, json=data, headers=headers)
    if response.status_code in (200, 201):
        return True
    else:
        print(response)
        return False

def count_targets(hostname):
    target_count = 0
    client_list_url = '%s/api/clients'  % (hostname)
    response = get(client_list_url)
    if response:
        for target in response:
            # perform get to make sure target is connected
            endpoint_url = '%s/api/clients/%s/3/0/2' % (hostname, target['endpoint'])
            endpoint_serial = get(endpoint_url)
            if endpoint_serial is not None:
                target_count += 1

    return target_count

def run(targets, hostname, num_loops, loop_delay, max_waits):
    global aborted
    global test_fail

    loop_counter = 0
    test_fail = False
    while aborted == False and (num_loops == 0 or loop_counter < num_loops):
        loop_counter += 1
        print("BEGIN Reset Loop %d" % loop_counter)

        # wait for connections
        run = True
        while run == True and aborted == False:
            target_count = count_targets(hostname)
            if target_count >= targets:
                print("  Found %d targets prior to reset" % targets)
                run = False
            else:
                print("  Only found %d targets, sleeping..." % target_count)
                time.sleep(5)

        if aborted == False:
            client_list_url = '%s/api/clients'  % (hostname)
            response = get(client_list_url)

        if aborted == False and response:
            print("  Resetting targets")
            for target in response:
                if 'endpoint' in target:
                    # send reset to targets
                    endpoint_url = '%s/api/clients/%s/3/0/4' % (hostname, target['endpoint'])
                    post(endpoint_url)

            if aborted == False:
                print("  Waiting for %d seconds post reset." % loop_delay)
                time.sleep(loop_delay)

            if aborted == False:
                # wait for connections again
                run = True
                wait_count = 0
                while run == True and aborted == False:
                    target_count = count_targets(hostname)
                    if target_count >= targets:
                        print("  Found %d targets after reset" % targets)
                        run = False
                    else:
                        wait_count += 1
                        if wait_count <= max_waits:
                            print("  Only found %d targets, sleeping... (%d waits left)" % (target_count, max_waits - wait_count))
                            time.sleep(5)
                        else:
                            aborted = True
                            test_fail = True

        if test_fail:
            print("END Reset Loop %d: FAILED!!" % loop_counter)
        elif aborted == True:
            print("END Reset Loop %d: ABORTED" % loop_counter)
        else:
            print("END Reset Loop %d: SUCCESS" % loop_counter)

    return loop_counter

def main():
    global aborted
    global test_fail

    # add process interrupt handler
    signal.signal(signal.SIGINT, signal_handler)

    description = 'Simple Leshan API Wrapper to wait for targets to connect and then reset them'
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('-t', '--targets', help='Number of Leshan Targets to wait for', type=int, required=True)
    parser.add_argument('-host', '--hostname', help='Leshan Server URL', default='https://mgmt.foundries.io/leshan')
    parser.add_argument('-l', '--loops', help='Number of loop executions', type=int, default=0)
    parser.add_argument('-d', '--delay', help='Wait delay between loops (in seconds)', type=int, default=45)
    parser.add_argument('-w', '--wait', help='Max waits for fail', type=int, default=6)
    args = parser.parse_args()
    loop_counter = run(args.targets, args.hostname, args.loops, args.delay, args.wait)
    print("---------------------")
    if test_fail:
        print("Failed during loop %d." % loop_counter)
        sys.exit(1)
    if aborted:
        print("Aborted during loop %d." % loop_counter)
        sys.exit(1)
    else:
        print("Successfully ran %d loop(s)." % loop_counter)
    sys.exit(0)

if __name__ == '__main__':
    main()
