# Copyright 2018 Brendan Duke.
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

"""Unit test for loadvid."""
import random
import time
import traceback
import click
import numpy as np
from PIL import Image
import lintel

width = 540
height = 964

filename = '1000016779.mp4'
filename = "http://ad.us.sinaimg.cn/003G3vKglx07yGAUM0ww010412004Z2L0E010.mp4?label=mp4_ld&template=320x568.25.0&trans_finger=6a03c1693f10c8f154facfffec859f78&Expires=1583567923&ssig=IC6pqXfr4U&KID=unistore,video"

start = time.perf_counter()
for _ in range(1):

    try:
        frame_count = lintel.frame_count(filename, timeout=1)
        print(frame_count)
    
        frame_nums = list(range(0, frame_count, int(frame_count/5)))    
        print(frame_nums)
        result = lintel.loadvid_frame_nums(filename,
                                            frame_nums=frame_nums,
                                            resize=224,
                                            should_key=True,
                                            timeout=1)
        decoded_frames, width, height = result
        print("width: ", width)
        print("height: ", height)

        decoded_frames = np.frombuffer(decoded_frames, dtype=np.uint8)
        decoded_frames = np.reshape(decoded_frames,
                                    newshape=(len(frame_nums), height, width, 3))
    except Exception as err:
        traceback.print_exc()
        print('python error: ', type(err))
    end = time.perf_counter()
    print('time: {}'.format(end - start))

for i in range(len(frame_nums)):
    frame = decoded_frames[i]
    Image.fromarray(frame).save('frame_idx_{}.jpeg'.format(frame_nums[i]))


# result = lintel.loadvid(filename,
#                         should_random_seek=True,
#                         num_frames=32)
# print(result[-3:])
