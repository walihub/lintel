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

import click
import numpy as np
from PIL import Image
import lintel

width = 540
height = 964

filename = '1000016779.mp4'
with open(filename, 'rb') as f:
    encoded_video = f.read()

    start = time.perf_counter()
    for _ in range(20):
        frame_count = lintel.frame_count(filename)
        print(frame_count)
        frame_nums = list(range(0, frame_count, int(frame_count/5)))
        # frame_nums = [400, 955]
        print(frame_nums)
        # frame_nums = [1, 20, 40]
        result = lintel.loadvid_frame_nums(filename,
                                            frame_nums=frame_nums,
                                            width=width,
                                            height=height,
                                            should_key=True,
                                            should_seek=True
                                            )
        
        # decoded_frames, width, height = result
        # print("width: ", width)
        # print("height: ", height)
        decoded_frames = result

        decoded_frames = np.frombuffer(decoded_frames, dtype=np.uint8)
        decoded_frames = np.reshape(decoded_frames,
                                    newshape=(len(frame_nums), height, width, 3))
    end = time.perf_counter()

    print('time: {}'.format((end - start)/20))

for i in range(len(frame_nums)):
    frame = decoded_frames[i]
    Image.fromarray(frame).save('frame_idx_{}.jpeg'.format(frame_nums[i]))
    # cv2.imwrite('loadvid_frame_index_{}.jpeg'.format(frame_index[i]), frame)

result = lintel.loadvid(filename,
                        should_random_seek=True,
                        num_frames=32)
print(result[-3:])
