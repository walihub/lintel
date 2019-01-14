import cv2
import random
import time
import click
import numpy as np
import lintel
import glob
import matplotlib.pyplot as plt


def set_frames_index(gop_num, num_seg):

    if gop_num > num_seg:
        tick = (gop_num) / float(num_seg)
        frames_index = [int(tick / 2.0 + tick * x) for x in range(num_seg)]
    else:
        frames_index = list(range(0, num_seg))

    return frames_index


video_file = '1018620873.mp4'
repet_num = 10
num_seg = 4
resize=224
count = 0

start = time.time()
for i in range(repet_num):
    a = time.time()
    count = count + 1
    try:
        gop_num = lintel.get_num_gops(video_file)
        frame_index = set_frames_index(gop_num, num_seg)
    except:
        print("error")
    decoded_frames, width, height = lintel.loadvid_frame_index(video_file, frame_nums=frame_index, resize=resize)
    decoded_frames = np.frombuffer(decoded_frames, dtype=np.uint8)
    decoded_frames = np.reshape(decoded_frames, newshape=(len(frame_index), height, width, 3))
    c = time.time()
end = time.time()
print(frame_index)
print('lintel: loadvid_frame_index time: {}'.format((end-start)/repet_num))

for i in range(len(frame_index)):
    frame = decoded_frames[i, ...]
    cv2.imwrite('loadvid_frame_index_{}.jpeg'.format(frame_index[i]), frame)







