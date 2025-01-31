#!/usr/bin/env python3
# Copyright (c) Glow Contributors. See CONTRIBUTORS file.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import argparse
import json
import numpy
import re
from collections import defaultdict
from operator import attrgetter, itemgetter


def formatUs(time):
    """ Format human readable time (input in us). """
    return f"{int(time)} us"
    if time < 1000:
        return f"{time:.2f} us"

    time = time / 1000
    if time < 1000:
        return f"{time:.2f} ms"

    time = time / 1000
    return f"{time:.2f} s"


class Event:
    """ Class to hold TraceEvents, matches glow::TraceEvent. """

    def __init__(self, name, start, end, optype):
        self.name = name
        self.start = start
        self.end = end
        self.optype = optype
        self.children = []
        self.child_time = 0

    def __repr__(self):
        return f"Event({self.name}, {self.start}, {self.end}, {self.optype})"

    def printTree(self, tabs):
        """ Pretty print the tree. """
        indent = tabs * "\t"
        print(f"{indent}{self.name} ({self.optype})")
        for c in self.children:
            c.printTree(tabs + 1)

    def totalOverlap(self, event):
        """ Returns True if this Event completely incloses the provided event. """
        return self.start <= event.start and self.end >= event.end

    def addChild(self, event):
        """ Add an enclosed event. """
        self.children.append(event)

    def updateChildTime(self):
        """ Determine the total time cost of all children. """
        self.child_time = 0
        for child in self.children:
            child.updateChildTime()
            self.child_time += child.end - child.start

    def selfTime(self):
        """ Return this Event's time cost above the sum of its children. """
        return (self.end - self.start) - self.child_time


def loadEvents(filename, runtimeEvents, fixedEvent, skip):
    """ Load the json trace file and create Events. """
    trace = None
    with open(filename) as f:
        trace = json.load(f)
    events = []
    partialEvents = {}
    for line in trace:
        if "name" in line:
            name = line["name"]
            evtype = line["ph"]
            start = int(line["ts"])
            optype = "runtime"
            if "args" in line:
                if "type" in line["args"]:
                    optype = line["args"]["type"]
                elif "kind" in line["args"]:
                    optype = line["args"]["kind"]

            # If we're looking for a single event, skip others.
            if fixedEvent and not re.match(fixedEvent, name) and not re.match(fixedEvent, optype):
                continue

            # if we're not including runtime events, skip them.
            if not fixedEvent and not runtimeEvents and optype == "runtime":
                continue

            # If we're skipping some number of events, skip them.
            if skip > 0:
                skip = skip - 1
                continue

            end = 0
            if evtype == "X":
                end = start + int(line["dur"])
                events.append(Event(name, start, end, optype))
            elif evtype == "B":
                partialEvents[name] = Event(name, start, end, optype)
            elif evtype == "E":
                if not name in partialEvents:
                    # This is a bug in Glow tracing, but ignore for now.
                    continue
                ev = partialEvents[name]
                ev.end = start
                events.append(ev)

    return events


def stackEvents(events):
    """ Find all enclosed events and move them to be children. Returns a tree of Events
        where parents completely enclose the timeline of their children. """
    # Ensure events are sorted by time.
    events = sorted(events, key=attrgetter("end"), reverse=True)
    events = sorted(events, key=attrgetter("start"))
    result = []
    lastEvent = None
    for ev in events:
        # If ev is enclosed by the previous event, add it as a child.
        if lastEvent:
            if lastEvent.totalOverlap(ev):
                lastEvent.addChild(ev)
                continue
            # If we're closing the previous event, recursively stack its children.
            if lastEvent.children:
                lastEvent.children = stackEvents(lastEvent.children)
                lastEvent.updateChildTime()
        # If not enclosed its a new top-level event, which may enclose other events.
        lastEvent = ev
        result.append(ev)
    # Stack children of the last Event.
    if lastEvent.children:
        lastEvent.children = stackEvents(lastEvent.children)
        lastEvent.updateChildTime()
    return result


def name_map(output_name):
    name_list = output_name.split('_')
    if 'linearbottleneck' in name_list[2]:
        name = 'b' + name_list[2].replace('linearbottleneck', '')
        if name_list[3] == 'conv0':
            name = name + "_expand"
        elif name_list[3] == 'conv1':
            name = name + "_dwise"
        else:
            name = name + "_linear"
    elif name_list[2] == 'conv0':
        name = 'conv0'
    elif name_list[2] == 'conv1':
        name = 'conv1'
    else:
        name = 'conv2'

    return name

def dumpAccumulate(events, keyfunc, traceTime):
    """ Accumulate Event durations by a key produced by keyfunc. Keyfunc is a lambda which
        takes an Event as a parameter. """
    nameMap = defaultdict(list)
    for ev in events:
        if 'conv' not in ev.optype:
            continue
        name = keyfunc(ev)
        nameMap[name].append(ev.selfTime())

    layers = []
    for (name, times) in nameMap.items():
        layers.append((name, len(times), numpy.mean(times),
                       numpy.std(times), numpy.sum(times)))

    # Iterate sorted by total time.
    for (name, num, mean, stddev, total) in layers:
    # for (name, num, mean, stddev, total) in sorted(layers,
    #                                                key=itemgetter(4), reverse=True):
        mean = formatUs(mean)
        stddev = formatUs(stddev)
        pc = (total / traceTime) * 100
        total = formatUs(total)
        print(
            f"{name} {num} events, mean: {mean}, stddev: {stddev}, total: {total} ({pc:.2f}%)")
    print()
    print()
    
    for (name, _, mean, _, _) in layers:
        name = name_map(name)
        mean = formatUs(mean)
        print(f"{name}, {mean}")


def resnet50_split(ox):
    node_list = [[str(ox.graph.node[0].output[0])], []]
    for node in ox.graph.node[1:]:
        if node.op_type == 'Conv':
            node_list[-1].append(str(node.output[0]) if int(node.input[0]) + 1 == int(node.output[0]) else str(node.output[0]) + '-shortcut')
        elif node.op_type == 'Add':
            node_list.append([])
    return node_list[:-1]


def resnet50_map(node_list):
    name_map = {node_list[0][0]: 'conv1'}
    conv_block = 1
    conv_layers = 1
    for node_block in node_list[1:]:
        if 'shortcut' in node_block[-1]:
            conv_block += 1
            conv_layers = 1
        conv_num = 1
        for node in node_block:
            if 'shortcut' not in node:
                name_map[node] = f"conv{conv_block}_x{conv_layers}_{conv_num}"
            else:
                name_map[node.split('-')[0]] = f"conv{conv_block}_x{conv_layers}_shortcut"
            conv_num += 1
        conv_layers += 1
    return name_map



def get_resnet50_map():
    import  onnx
    
    ox = onnx.load('resnet50.onnx')
    node_split = resnet50_split(ox)
    node_map = resnet50_map(node_split)
    return node_map

    


def dump_trace_chronological(events, keyfunc, traceTime):
    nameMap = defaultdict(list)
    for ev in events:
        if 'conv' not in ev.optype:
            continue
        name = keyfunc(ev)
        nameMap[name].append(ev.selfTime())

    layers = []
    for (name, times) in nameMap.items():
        layers.append((name, len(times), numpy.mean(times), numpy.std(times), numpy.sum(times)))

    for (name, num, mean, stddev, total) in layers:
        mean =  formatUs(mean)
        stddev = formatUs(stddev)
        pc = (total / traceTime) * 100
        total = formatUs(total)
        print(f"{name} {num} events, mean: {mean}, stddev: {stddev}, total: {total} ({pc:.2f}%)")

    print()
    print()

    output_map = get_resnet50_map()
    
    layer_time = defaultdict(list)
    for (name, _, mean, _, _) in layers:
        output_name = output_map[name.split('_')[1]]
        print(f"{output_name} {mean} us")
        conv_block = output_name.split('_')[0]
        conv_num = output_name.split('_')[-1]
        layer_time[conv_block + '_' + conv_num].append(int(mean) / 1000)

    for layer_name, time_list in layer_time.items():
        print(f'{layer_name}, {time_list}')




def main():
    parser = argparse.ArgumentParser(description='process trace json')
    parser.add_argument('filename', type=str,
                        help='filename for trace file to load')
    parser.add_argument("--mobilenet", action='store_true',
                        help="aggregate and display by layer names")
    parser.add_argument("--summarize", action='store_true',
                        help="print a summary of the trace")
    parser.add_argument('--resnet50', action='store_true',
                        help='display by resnet50 layers')

    args = parser.parse_args()
    events = loadEvents(args.filename, args.runtime, args.event, args.skip)
    if not events:
        return

    # Stack events so we can determine selfTime.
    stacked = stackEvents(events)

    # Ensure events are sorted by startTime.
    stacked = sorted(stacked, key=attrgetter("start"))
    totalTime = stacked[-1].end - stacked[0].start
    coveredTime = 0
    for ev in stacked:
        coveredTime += ev.end - ev.start

    if args.mobilenet:
        dumpAccumulate(events, lambda ev: f"{ev.name} ({ev.optype})",
                       coveredTime)

    if args.resnet50:
        dump_trace_chronological(events, lambda ev: f"{ev.name} ({ev.optype})", coveredTime)

    if args.summarize:
        print("Total time of trace:", formatUs(totalTime))
        print("Time covered by events:", formatUs(coveredTime))
        print("Unattributed time:", formatUs(totalTime - coveredTime))


if __name__ == "__main__":
    main()
