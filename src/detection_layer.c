#include "detection_layer.h"
#include "activations.h"
#include "softmax_layer.h"
#include "blas.h"
#include "box.h"
#include "dark_cuda.h"
#include "utils.h"
#include <stdio.h>
#include <assert.h>
#include <string.h>
#include <stdlib.h>

detection_layer make_detection_layer(int batch, int inputs, int n, int side, int classes, int coords, int rescore)
{
    detection_layer l = { (LAYER_TYPE)0 };
    l.type = DETECTION;     // set layer type

    l.n = n;                // number of bounding boxes in each grid cell
    l.batch = batch;        // ?? batch size
    l.inputs = inputs;      // the output size (the 3d matrix) of each image
    l.classes = classes;    // number of classification categories, depends the value in the cfg file
    l.coords = coords;      // number of bounding box's coords (w, h, s, w), so l.coords is 4
    l.rescore = rescore;
    l.side = side;          // number of grid cells in each side
    l.w = side;
    l.h = side;

    // to make sure `inputs`'s value is correct
    assert(side*side*((1 + l.coords)*l.n + l.classes) == inputs);

    l.cost = (float*)calloc(1, sizeof(float));                  // to store the cost function value
    l.outputs = l.inputs;
    l.truths = l.side*l.side*(1+l.coords+l.classes);            // ground truth bounding boxes for all grid cells
    l.output = (float*)calloc(batch * l.outputs, sizeof(float));// the memory for all batch's output
    l.delta = (float*)calloc(batch * l.outputs, sizeof(float)); // the loss delta, used to calculate the final cost

    l.forward = forward_detection_layer;
    l.backward = backward_detection_layer;
#ifdef GPU
    l.forward_gpu = forward_detection_layer_gpu;
    l.backward_gpu = backward_detection_layer_gpu;
    l.output_gpu = cuda_make_array(l.output, batch*l.outputs);
    l.delta_gpu = cuda_make_array(l.delta, batch*l.outputs);
#endif

    fprintf(stderr, "Detection Layer\n");
    srand(time(0));

    return l;
}

void forward_detection_layer(const detection_layer l, network_state state)
{
    int locations = l.side*l.side;
    int i,j;
    memcpy(l.output, state.input, l.outputs*l.batch*sizeof(float));
    //if(l.reorg) reorg(l.output, l.w*l.h, size*l.n, l.batch, 1);
    int b;
    if (l.softmax){
        for(b = 0; b < l.batch; ++b){
            int index = b*l.inputs;
            for (i = 0; i < locations; ++i) {
                int offset = i*l.classes;
                softmax(l.output + index + offset, l.classes, 1,
                        l.output + index + offset, 1);
            }
        }
    }

    // cost function is only for training stage
    if(state.train){
        float avg_iou = 0;
        float avg_cat = 0;
        float avg_allcat = 0;
        float avg_obj = 0;
        float avg_anyobj = 0;
        int count = 0;
        *(l.cost) = 0;
        int size = l.inputs * l.batch;
        memset(l.delta, 0, size * sizeof(float));

        // loop each batch
        for (b = 0; b < l.batch; ++b){
            int index = b*l.inputs;

            // loop all grid cells, e.g., locations = 7*7 = 49
            for (i = 0; i < locations; ++i) {
                // get the ground truth bounding box's offset
                int truth_index = (b*locations + i)*(1+l.coords+l.classes);

                // whether a grid cell contains the center of an object. (whether `truth[truth_index]` is NULL)
                int is_obj = state.truth[truth_index];

                // 1. calculate IOU ERROR
                // loop `l.n` bounding boxes for each grid cell (from cfg file, e.g., num=5)
                for (j = 0; j < l.n; ++j) { 
                    int p_index = index + locations*l.classes + i*l.n + j;
                    
                    // assume there's no object in each grid cell and calculate the IOU cost
                    // the grid cells which contain objects, extra cost will be deducted later
                    l.delta[p_index] = l.noobject_scale*(0 - l.output[p_index]);

                    avg_anyobj += l.output[p_index];
                }

                int best_index = -1;
                float best_iou = 0;
                float best_rmse = 20;

                /* if there is no object in current grid cell, return!
                   actually, for most grid cells, no object exists. */
                if (!is_obj){
                    continue;
                }

                // 2. calculate Classification ERROR
                int class_index = index + i*l.classes;
                for(j = 0; j < l.classes; ++j) {
                    l.delta[class_index+j] = l.class_scale * (state.truth[truth_index+1+j] - l.output[class_index+j]);
                    
                    if(state.truth[truth_index + 1 + j]) {
                        avg_cat += l.output[class_index+j];
                    }
                    avg_allcat += l.output[class_index+j];
                }

                // get the ground truth bounding box of current grid cell's object
                box truth = float_to_box(state.truth + truth_index + 1 + l.classes);
                truth.x /= l.side;  // ??
                truth.y /= l.side;  // ??

                // loop all bounding boxes, find the one who is "resposible" for object detection
                for(j = 0; j < l.n; ++j){
                    int box_index = index + locations*(l.classes + l.n) + (i*l.n + j) * l.coords;
                    box out = float_to_box(l.output + box_index);
                    out.x /= l.side;
                    out.y /= l.side;

                    if (l.sqrt){
                        out.w = out.w*out.w;
                        out.h = out.h*out.h;
                    }

                    // calculate IOU between current and the ground truth bounding boxes
                    float iou  = box_iou(out, truth);
                    // calculate RMSE (root-mean-square error)
                    float rmse = box_rmse(out, truth);

                    // find the best bounding box with the max(IOU), if all IOU is 0, find min(RMSE)
                    if(best_iou > 0 || iou > 0){
                        if(iou > best_iou){
                            best_iou = iou;
                            best_index = j;
                        }
                    }else{
                        if(rmse < best_rmse){
                            best_rmse = rmse;
                            best_index = j;
                        }
                    }
                }

                // `forced` is 0 by default
                if(l.forced){
                    if(truth.w*truth.h < .1){
                        best_index = 1;
                    }else{
                        best_index = 0;
                    }
                }
                // `random` is 0 by default
                if(l.random && *(state.net.seen) < 64000){
                    best_index = rand()%l.n;
                }

                // get best (regarding IOU/RMSE) bounding box's coords offset in `l.output`'s memory
                int box_index = index + locations*(l.classes + l.n) + (i*l.n + best_index) * l.coords;
                // get current grid cell's gound truth bounding box's offset in `state.truth`'s memory
                int tbox_index = truth_index + 1 + l.classes;

                box out = float_to_box(l.output + box_index);
                out.x /= l.side;
                out.y /= l.side;
                if (l.sqrt) {
                    out.w = out.w*out.w;
                    out.h = out.h*out.h;
                }
                float iou  = box_iou(out, truth);

                //printf("%d,", best_index);

                // 3. calculate IOU ERROR
                // get best (regarding IOU/RMSE) bounding box's confidence offset in `l.output`'s memory
                int p_index = index + locations*l.classes + i*l.n + best_index;
                avg_obj += l.output[p_index];

                l.delta[p_index] = l.object_scale * (1.-l.output[p_index]);
                if(l.rescore){
                    l.delta[p_index] = l.object_scale * (iou - l.output[p_index]);
                }

                // 4. calculate Coordinates ERROR
                l.delta[box_index+0] = l.coord_scale*(state.truth[tbox_index + 0] - l.output[box_index + 0]);
                l.delta[box_index+1] = l.coord_scale*(state.truth[tbox_index + 1] - l.output[box_index + 1]);
                l.delta[box_index+2] = l.coord_scale*(state.truth[tbox_index + 2] - l.output[box_index + 2]);
                l.delta[box_index+3] = l.coord_scale*(state.truth[tbox_index + 3] - l.output[box_index + 3]);
                if(l.sqrt){
                    l.delta[box_index+2] = l.coord_scale*(sqrt(state.truth[tbox_index + 2]) - l.output[box_index + 2]);
                    l.delta[box_index+3] = l.coord_scale*(sqrt(state.truth[tbox_index + 3]) - l.output[box_index + 3]);
                }

                avg_iou += iou;
                ++count;
            }
        }

        // calculate the final cost with `l.delta` (sum of squares)
        *(l.cost) = pow(mag_array(l.delta, l.outputs * l.batch), 2);

        printf("Detection Avg IOU: %f, Pos Cat: %f, All Cat: %f, Pos Obj: %f, Any Obj: %f, count: %d\n", avg_iou/count, avg_cat/count, avg_allcat/(count*l.classes), avg_obj/count, avg_anyobj/(l.batch*locations*l.n), count);
        //if(l.reorg) reorg(l.delta, l.w*l.h, size*l.n, l.batch, 0);
    }
}

void backward_detection_layer(const detection_layer l, network_state state)
{
    // add the correspond values in l.delta to state.delta
    axpy_cpu(l.batch*l.inputs, 1, l.delta, 1, state.delta, 1);
}

void get_detection_boxes(layer l, int w, int h, float thresh, float **probs, box *boxes, int only_objectness)
{
    int i,j,n;
    float *predictions = l.output;
    //int per_cell = 5*num+classes;
    for (i = 0; i < l.side*l.side; ++i){
        int row = i / l.side;
        int col = i % l.side;
        for(n = 0; n < l.n; ++n){
            int index = i*l.n + n;
            int p_index = l.side*l.side*l.classes + i*l.n + n;
            float scale = predictions[p_index];
            int box_index = l.side*l.side*(l.classes + l.n) + (i*l.n + n)*4;
            boxes[index].x = (predictions[box_index + 0] + col) / l.side * w;
            boxes[index].y = (predictions[box_index + 1] + row) / l.side * h;
            boxes[index].w = pow(predictions[box_index + 2], (l.sqrt?2:1)) * w;
            boxes[index].h = pow(predictions[box_index + 3], (l.sqrt?2:1)) * h;
            for(j = 0; j < l.classes; ++j){
                int class_index = i*l.classes;
                float prob = scale*predictions[class_index+j];
                probs[index][j] = (prob > thresh) ? prob : 0;
            }
            if(only_objectness){
                probs[index][0] = scale;
            }
        }
    }
}

#ifdef GPU

void forward_detection_layer_gpu(const detection_layer l, network_state state)
{
    if(!state.train){
        copy_ongpu(l.batch*l.inputs, state.input, 1, l.output_gpu, 1);
        return;
    }

    float* in_cpu = (float*)calloc(l.batch * l.inputs, sizeof(float));
    float *truth_cpu = 0;
    if(state.truth){
        int num_truth = l.batch*l.side*l.side*(1+l.coords+l.classes);
        truth_cpu = (float*)calloc(num_truth, sizeof(float));
        cuda_pull_array(state.truth, truth_cpu, num_truth);
    }
    cuda_pull_array(state.input, in_cpu, l.batch*l.inputs);
    network_state cpu_state = state;
    cpu_state.train = state.train;
    cpu_state.truth = truth_cpu;
    cpu_state.input = in_cpu;
    forward_detection_layer(l, cpu_state);
    cuda_push_array(l.output_gpu, l.output, l.batch*l.outputs);
    cuda_push_array(l.delta_gpu, l.delta, l.batch*l.inputs);
    free(cpu_state.input);
    if(cpu_state.truth) free(cpu_state.truth);
}

void backward_detection_layer_gpu(detection_layer l, network_state state)
{
    axpy_ongpu(l.batch*l.inputs, 1, l.delta_gpu, 1, state.delta, 1);
    //copy_ongpu(l.batch*l.inputs, l.delta_gpu, 1, state.delta, 1);
}
#endif

void get_detection_detections(layer l, int w, int h, float thresh, detection *dets)
{
	int i, j, n;
	float *predictions = l.output;
	//int per_cell = 5*num+classes;
	for (i = 0; i < l.side*l.side; ++i) {
		int row = i / l.side;
		int col = i % l.side;
		for (n = 0; n < l.n; ++n) {
			int index = i*l.n + n;
			int p_index = l.side*l.side*l.classes + i*l.n + n;
			float scale = predictions[p_index];
			int box_index = l.side*l.side*(l.classes + l.n) + (i*l.n + n) * 4;
			box b;
			b.x = (predictions[box_index + 0] + col) / l.side * w;
			b.y = (predictions[box_index + 1] + row) / l.side * h;
			b.w = pow(predictions[box_index + 2], (l.sqrt ? 2 : 1)) * w;
			b.h = pow(predictions[box_index + 3], (l.sqrt ? 2 : 1)) * h;
			dets[index].bbox = b;
			dets[index].objectness = scale;
			for (j = 0; j < l.classes; ++j) {
				int class_index = i*l.classes;
				float prob = scale*predictions[class_index + j];
				dets[index].prob[j] = (prob > thresh) ? prob : 0;
			}
		}
	}
}
