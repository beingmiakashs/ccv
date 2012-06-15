#include "ccv.h"
#include "ccv_internal.h"
#include <sys/time.h>
#ifdef HAVE_GSL
#include <gsl/gsl_rng.h>
#include <gsl/gsl_randist.h>
#endif
#ifdef USE_OPENMP
#include <omp.h>
#endif
#ifdef HAVE_LIBLINEAR
#include <linear.h>
#endif

#define CCV_DPM_WINDOW_SIZE (8)

static unsigned int _ccv_dpm_time_measure()
{
	struct timeval tv;
	gettimeofday(&tv, 0);
	return tv.tv_sec * 1000000 + tv.tv_usec;
}

static int _ccv_dpm_scale_upto(ccv_dense_matrix_t* a, ccv_dpm_mixture_model_t** _model, int count, int interval)
{
	int c, i;
	ccv_size_t size = ccv_size(a->cols, a->rows);
	for (c = 0; c < count; c++)
	{
		ccv_dpm_mixture_model_t* model = _model[c];
		for (i = 0; i < model->count; i++)
		{
			size.width = ccv_min(model->root[i].root.w->cols * CCV_DPM_WINDOW_SIZE, size.width);
			size.height = ccv_min(model->root[i].root.w->rows * CCV_DPM_WINDOW_SIZE, size.height);
		}
	}
	int hr = a->rows / size.height;
	int wr = a->cols / size.width;
	double scale = pow(2.0, 1.0 / (interval + 1.0));
	int next = interval + 1;
	return (int)(log((double)ccv_min(hr, wr)) / log(scale)) - next;
}

static void _ccv_dpm_feature_pyramid(ccv_dense_matrix_t* a, ccv_dense_matrix_t** pyr, int scale_upto, int interval)
{
	int next = interval + 1;
	double scale = pow(2.0, 1.0 / (interval + 1.0));
	memset(pyr, 0, (scale_upto + next * 2) * sizeof(ccv_dense_matrix_t*));
	pyr[next] = a;
	int i;
	for (i = 1; i <= interval; i++)
		ccv_resample(pyr[next], &pyr[next + i], 0, (int)(pyr[next]->rows / pow(scale, i)), (int)(pyr[next]->cols / pow(scale, i)), CCV_INTER_AREA);
	for (i = next; i < scale_upto + next; i++)
		ccv_sample_down(pyr[i], &pyr[i + next], 0, 0, 0);
	ccv_dense_matrix_t* hog;
	/* a more efficient way to generate up-scaled hog (using smaller size) */
	for (i = 0; i < next; i++)
	{
		hog = 0;
		ccv_hog(pyr[i + next], &hog, 0, 9, CCV_DPM_WINDOW_SIZE / 2 /* this is */);
		pyr[i] = hog;
	}
	hog = 0;
	ccv_hog(pyr[next], &hog, 0, 9, CCV_DPM_WINDOW_SIZE);
	pyr[next] = hog;
	for (i = next + 1; i < scale_upto + next * 2; i++)
	{
		hog = 0;
		ccv_hog(pyr[i], &hog, 0, 9, CCV_DPM_WINDOW_SIZE);
		ccv_matrix_free(pyr[i]);
		pyr[i] = hog;
	}
}

static void _ccv_dpm_compute_score(ccv_dpm_root_classifier_t* root_classifier, ccv_dense_matrix_t* hog, ccv_dense_matrix_t* hog2x, ccv_dense_matrix_t** _response, ccv_dense_matrix_t** part_feature, ccv_dense_matrix_t** dx, ccv_dense_matrix_t** dy)
{
	ccv_dense_matrix_t* response = 0;
	ccv_filter(hog, root_classifier->root.w, &response, 0, CCV_NO_PADDING);
	ccv_dense_matrix_t* root_feature = 0;
	ccv_flatten(response, (ccv_matrix_t**)&root_feature, 0, 0);
	ccv_matrix_free(response);
	*_response = root_feature;
	if (hog2x == 0)
		return;
	ccv_make_matrix_mutable(root_feature);
	int rwh = (root_classifier->root.w->rows - 1) / 2;
	int rww = (root_classifier->root.w->cols - 1) / 2;
	int i, x, y;
	for (i = 0; i < root_classifier->count; i++)
	{
		ccv_dpm_part_classifier_t* part = root_classifier->part + i;
		ccv_dense_matrix_t* response = 0;
		ccv_filter(hog2x, part->w, &response, 0, CCV_NO_PADDING);
		ccv_dense_matrix_t* feature = 0;
		ccv_flatten(response, (ccv_matrix_t**)&feature, 0, 0);
		ccv_matrix_free(response);
		part_feature[i] = dx[i] = dy[i] = 0;
		ccv_distance_transform(feature, &part_feature[i], 0, &dx[i], 0, &dy[i], 0, part->dx, part->dy, part->dxx, part->dyy, CCV_NEGATIVE | CCV_GSEDT);
		ccv_matrix_free(feature);
		int pwh = (part->w->rows - 1) / 2, pww = (part->w->cols - 1) / 2;
		int offy = part->y + pwh - rwh * 2;
		int miny = pwh, maxy = part_feature[i]->rows - part->w->rows + pwh;
		int offx = part->x + pww - rww * 2;
		int minx = pww, maxx = part_feature[i]->cols - part->w->cols + pww;
		float* f_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | CCV_C1, root_feature, rwh, 0, 0);
		for (y = rwh; y < root_feature->rows - rwh; y++)
		{
			int iy = ccv_clamp(y * 2 + offy, miny, maxy);
			for (x = rww; x < root_feature->cols - rww; x++)
			{
				int ix = ccv_clamp(x * 2 + offx, minx, maxx);
				f_ptr[x] -= ccv_get_dense_matrix_cell_value_by(CCV_32F | CCV_C1, part_feature[i], iy, ix, 0);
			}
			f_ptr += root_feature->cols;
		}
	}
}

#define less_than(fn1, fn2, aux) ((fn1).value >= (fn2).value)
static CCV_IMPLEMENT_QSORT(_ccv_dpm_aspect_qsort, struct feature_node, less_than)
#undef less_than

#define less_than(a1, a2, aux) ((a1) < (a2))
static CCV_IMPLEMENT_QSORT(_ccv_dpm_area_qsort, int, less_than)
#undef less_than

#define less_than(s1, s2, aux) ((s1) < (s2))
static CCV_IMPLEMENT_QSORT(_ccv_dpm_score_qsort, double, less_than)
#undef less_than

static ccv_dpm_mixture_model_t* _ccv_dpm_model_copy(ccv_dpm_mixture_model_t* _model)
{
	ccv_dpm_mixture_model_t* model = (ccv_dpm_mixture_model_t*)ccmalloc(sizeof(ccv_dpm_mixture_model_t));
	model->count = _model->count;
	model->root = (ccv_dpm_root_classifier_t*)ccmalloc(sizeof(ccv_dpm_root_classifier_t) * model->count);
	int i, j;
	memcpy(model->root, _model->root, sizeof(ccv_dpm_root_classifier_t) * model->count);
	for (i = 0; i < model->count; i++)
	{
		ccv_dpm_root_classifier_t* _root = _model->root + i;
		ccv_dpm_root_classifier_t* root = model->root + i;
		root->root.w = ccv_dense_matrix_new(_root->root.w->rows, _root->root.w->cols, CCV_32F | 31, 0, 0);
		memcpy(root->root.w->data.u8, _root->root.w->data.u8, _root->root.w->rows * _root->root.w->step);
		ccv_make_matrix_immutable(root->root.w);
		ccv_dpm_part_classifier_t* _part = _root->part;
 		ccv_dpm_part_classifier_t* part = root->part = (ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * root->count);
		memcpy(part, _part, sizeof(ccv_dpm_part_classifier_t) * root->count);
		for (j = 0; j < root->count; j++)
		{
			part[j].w = ccv_dense_matrix_new(_part[j].w->rows, _part[j].w->cols, CCV_32F | 31, 0, 0);
			memcpy(part[j].w->data.u8, _part[j].w->data.u8, _part[j].w->rows * _part[j].w->step);
			ccv_make_matrix_immutable(part[j].w);
		}
	}
	return model;
}

static void _ccv_dpm_write_checkpoint(ccv_dpm_mixture_model_t* model, const char* dir)
{
	char swpfile[1024];
	sprintf(swpfile, "%s.swp", dir);
	FILE* w = fopen(swpfile, "w+");
	if (!w)
		return;
	fprintf(w, ",\n");
	int i, j, x, y, ch, count = 0;
	for (i = 0; i < model->count; i++)
	{
		if (model->root[i].root.w == 0)
			break;
		count++;
	}
	fprintf(w, "%d %d\n", model->count, count);
	for (i = 0; i < count; i++)
	{
		ccv_dpm_root_classifier_t* root_classifier = model->root + i;
		fprintf(w, "%d %d\n", root_classifier->root.w->rows, root_classifier->root.w->cols);
		fprintf(w, "%a\n", root_classifier->beta);
		ch = CCV_GET_CHANNEL(root_classifier->root.w->type);
		for (y = 0; y < root_classifier->root.w->rows; y++)
		{
			for (x = 0; x < root_classifier->root.w->cols * ch; x++)
				fprintf(w, "%a ", root_classifier->root.w->data.f32[y * root_classifier->root.w->cols * ch + x]);
			fprintf(w, "\n");
		}
		fprintf(w, "%d\n", root_classifier->count);
		for (j = 0; j < root_classifier->count; j++)
		{
			ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + j;
			fprintf(w, "%d %d %d\n", part_classifier->x, part_classifier->y, part_classifier->z);
			fprintf(w, "%la %la %la %la\n", part_classifier->dx, part_classifier->dy, part_classifier->dxx, part_classifier->dyy);
			fprintf(w, "%d %d %d\n", part_classifier->w->rows, part_classifier->w->cols, part_classifier->counterpart);
			ch = CCV_GET_CHANNEL(part_classifier->w->type);
			for (y = 0; y < part_classifier->w->rows; y++)
			{
				for (x = 0; x < part_classifier->w->cols * ch; x++)
					fprintf(w, "%a ", part_classifier->w->data.f32[y * part_classifier->w->cols * ch + x]);
				fprintf(w, "\n");
			}
		}
	}
	fclose(w);
	rename(swpfile, dir);
}

static void _ccv_dpm_read_checkpoint(ccv_dpm_mixture_model_t* model, const char* dir)
{
	FILE* r = fopen(dir, "r");
	if (!r)
		return;
	int count;
	char flag;
	fscanf(r, "%c", &flag);
	assert(flag == ',');
	fscanf(r, "%d %d", &model->count, &count);
	ccv_dpm_root_classifier_t* root_classifier = (ccv_dpm_root_classifier_t*)ccmalloc(sizeof(ccv_dpm_root_classifier_t) * count);
	memset(root_classifier, 0, sizeof(ccv_dpm_root_classifier_t) * count);
	int i, j, k;
	for (i = 0; i < count; i++)
	{
		int rows, cols;
		fscanf(r, "%d %d", &rows, &cols);
		fscanf(r, "%f", &root_classifier[i].beta);
		root_classifier[i].root.w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, 0, 0);
		for (j = 0; j < rows * cols * 31; j++)
			fscanf(r, "%f", &root_classifier[i].root.w->data.f32[j]);
		ccv_make_matrix_immutable(root_classifier[i].root.w);
		fscanf(r, "%d", &root_classifier[i].count);
		ccv_dpm_part_classifier_t* part_classifier = (ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * root_classifier[i].count);
		for (j = 0; j < root_classifier[i].count; j++)
		{
			fscanf(r, "%d %d %d", &part_classifier[j].x, &part_classifier[j].y, &part_classifier[j].z);
			fscanf(r, "%lf %lf %lf %lf", &part_classifier[j].dx, &part_classifier[j].dy, &part_classifier[j].dxx, &part_classifier[j].dyy);
			fscanf(r, "%d %d %d", &rows, &cols, &part_classifier[j].counterpart);
			part_classifier[j].w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, 0, 0);
			for (k = 0; k < rows * cols * 31; k++)
				fscanf(r, "%f", &part_classifier[j].w->data.f32[k]);
			ccv_make_matrix_immutable(part_classifier[j].w);
		}
		root_classifier[i].part = part_classifier;
	}
	model->root = root_classifier;
	fclose(r);
}

static void _ccv_dpm_mixture_model_cleanup(ccv_dpm_mixture_model_t* model)
{
	/* this is different because it doesn't compress to a continuous memory region */
	int i, j;
	for (i = 0; i < model->count; i++)
	{
		ccv_dpm_root_classifier_t* root_classifier = model->root + i;
		for (j = 0; j < root_classifier->count; j++)
		{
			ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + j;
			ccv_matrix_free(part_classifier->w);
		}
		if (root_classifier->count > 0)
			ccfree(root_classifier->part);
		if (root_classifier->root.w != 0)
			ccv_matrix_free(root_classifier->root.w);
	}
	ccfree(model->root);
	model->count = 0;
	model->root = 0;
}

static const int _ccv_dpm_sym_lut[] = { 2, 3, 0, 1,
										4 + 0, 4 + 8, 4 + 7, 4 + 6, 4 + 5, 4 + 4, 4 + 3, 4 + 2, 4 + 1,
										13 + 9, 13 + 8, 13 + 7, 13 + 6, 13 + 5, 13 + 4, 13 + 3, 13 + 2, 13 + 1, 13, 13 + 17, 13 + 16, 13 + 15, 13 + 14, 13 + 13, 13 + 12, 13 + 11, 13 + 10 };

static void _ccv_dpm_check_root_classifier_symmetry(ccv_dense_matrix_t* w)
{
	assert(CCV_GET_CHANNEL(w->type) == 31 && CCV_GET_DATA_TYPE(w->type) == CCV_32F);
	float *w_ptr = w->data.f32;
	int i, j, k;
	for (i = 0; i < w->rows; i++)
	{
		for (j = 0; j < w->cols; j++)
		{
			for (k = 0; k < 31; k++)
			{
				double v = fabs(w_ptr[j * 31 + k] - w_ptr[(w->cols - 1 - j) * 31 + _ccv_dpm_sym_lut[k]]);
				if (v > 0.002)
					printf("symmetric violation at (%d, %d, %d), off by: %f\n", i, j, k, v);
			}
		}
		w_ptr += w->cols * 31;
	}
}

static void _ccv_dpm_initialize_root_classifier(gsl_rng* rng, ccv_dpm_root_classifier_t* root_classifier, int label, int cnum, int* labels, char** posfiles, ccv_rect_t* bboxes, int posnum, char** bgfiles, int bgnum, int negnum, double C, int symmetric, int grayscale)
{
	int i, j, x, y, k, l, n;
	int cols = root_classifier->root.w->cols;
	int cols2c = (cols + 1) / 2;
	int rows = root_classifier->root.w->rows;
	printf(" - creating initial model %d at %dx%d\n", label + 1, cols, rows);
	struct problem prob;
	prob.n = symmetric ? 31 * cols2c * rows + 1 : 31 * cols * rows + 1;
	prob.bias = symmetric ? 0.5 : 1.0; // for symmetric, since we only pass half features in, need to set bias to be half too
	prob.y = (int*)malloc(sizeof(int) * (cnum + negnum) * (!!symmetric + 1));
	prob.x = (struct feature_node**)malloc(sizeof(struct feature_node*) * (cnum + negnum) * (!!symmetric + 1));
	printf(" - generating positive examples ");
	fflush(stdout);
	l = 0;
	for (i = 0; i < posnum; i++)
		if (labels[i] == label)
		{
			ccv_rect_t bbox = bboxes[i];
			int mcols = (int)(sqrtf(bbox.width * bbox.height * cols / (float)rows) + 0.5);
			int mrows = (int)(sqrtf(bbox.width * bbox.height * rows / (float)cols) + 0.5);
			bbox.x = bbox.x + (bbox.width - mcols) / 2;
			bbox.y = bbox.y + (bbox.height - mrows) / 2;
			bbox.width = mcols;
			bbox.height = mrows;
			if (mcols * 2 < cols * CCV_DPM_WINDOW_SIZE || mrows * 2 < rows * CCV_DPM_WINDOW_SIZE)
			// resolution is too low to be useful
				continue;
			ccv_dense_matrix_t* image = 0;
			ccv_read(posfiles[i], &image, (grayscale ? CCV_IO_GRAY : 0) | CCV_IO_ANY_FILE);
			assert(image != 0);
			ccv_dense_matrix_t* up2x = 0;
			ccv_sample_up(image, &up2x, 0, 0, 0);
			ccv_matrix_free(image);
			ccv_dense_matrix_t* slice = 0;
			ccv_slice(up2x, (ccv_matrix_t**)&slice, 0, bbox.y * 2, bbox.x * 2, bbox.height * 2, bbox.width * 2);
			ccv_matrix_free(up2x);
			ccv_dense_matrix_t* resize = 0;
			ccv_resample(slice, &resize, 0, rows * CCV_DPM_WINDOW_SIZE, cols * CCV_DPM_WINDOW_SIZE, CCV_INTER_AREA);
			ccv_matrix_free(slice);
			ccv_dense_matrix_t* hog = 0;
			ccv_hog(resize, &hog, 0, 9, CCV_DPM_WINDOW_SIZE);
			ccv_matrix_free(resize);
			struct feature_node* features;
			if (symmetric)
			{
				features = (struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols2c * rows + 2));
				float* hptr = hog->data.f32;
				j = 0;
				for (y = 0; y < rows; y++)
				{
					for (x = 0; x < cols2c; x++)
						for (k = 0; k < 31; k++)
						{
							features[j].index = j + 1;
							features[j].value = hptr[x * 31 + k];
							++j;
						}
					hptr += hog->cols * 31;
				}
				features[j].index = j + 1;
				features[j].value = prob.bias;
				features[j + 1].index = -1;
				prob.x[l] = features;
				prob.y[l] = 1;
				++l;
				features = (struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols2c * rows + 2));
				hptr = hog->data.f32;
				j = 0;
				for (y = 0; y < rows; y++)
				{
					for (x = 0; x < cols2c; x++)
						for (k = 0; k < 31; k++)
						{
							features[j].index = j + 1;
							features[j].value = hptr[(cols - 1 - x) * 31 + _ccv_dpm_sym_lut[k]];
							++j;
						}
					hptr += hog->cols * 31;
				}
				features[j].index = j + 1;
				features[j].value = prob.bias;
				features[j + 1].index = -1;
				prob.x[l] = features;
				prob.y[l] = 1;
				++l;
			} else {
				features = (struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols * rows + 2));
				for (j = 0; j < rows * cols * 31; j++)
				{
					features[j].index = j + 1;
					features[j].value = hog->data.f32[j];
				}
				features[31 * rows * cols].index = 31 * rows * cols + 1;
				features[31 * rows * cols].value = prob.bias;
				features[31 * rows * cols + 1].index = -1;
				prob.x[l] = features;
				prob.y[l] = 1;
				++l;
			}
			ccv_matrix_free(hog);
			printf(".");
			fflush(stdout);
		}
	printf("\n - generating negative examples ");
	fflush(stdout);
	n = 0;
	while (n < negnum)
	{
		double p = (double)negnum / (double)bgnum;
		for (i = 0; i < bgnum; i++)
			if (gsl_rng_uniform(rng) < p)
			{
				ccv_dense_matrix_t* image = 0;
				ccv_read(bgfiles[i], &image, (grayscale ? CCV_IO_GRAY : 0) | CCV_IO_ANY_FILE);
				assert(image != 0);
				ccv_dense_matrix_t* slice = 0;
				int y = gsl_rng_uniform_int(rng, image->rows - rows * CCV_DPM_WINDOW_SIZE);
				int x = gsl_rng_uniform_int(rng, image->cols - cols * CCV_DPM_WINDOW_SIZE);
				ccv_slice(image, (ccv_matrix_t**)&slice, 0, y, x, rows * CCV_DPM_WINDOW_SIZE, cols * CCV_DPM_WINDOW_SIZE);
				ccv_matrix_free(image);
				ccv_dense_matrix_t* hog = 0;
				ccv_hog(slice, &hog, 0, 9, CCV_DPM_WINDOW_SIZE);
				ccv_matrix_free(slice);
				struct feature_node* features;
				if (symmetric)
				{
					features = (struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols2c * rows + 2));
					float* hptr = hog->data.f32;
					j = 0;
					for (y = 0; y < rows; y++)
					{
						for (x = 0; x < cols2c; x++)
							for (k = 0; k < 31; k++)
							{
								features[j].index = j + 1;
								features[j].value = hptr[x * 31 + k];
								++j;
							}
						hptr += hog->cols * 31;
					}
					features[j].index = j + 1;
					features[j].value = prob.bias;
					features[j + 1].index = -1;
					prob.x[l] = features;
					prob.y[l] = -1;
					++l;
					features = (struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols2c * rows + 2));
					hptr = hog->data.f32;
					j = 0;
					for (y = 0; y < rows; y++)
					{
						for (x = 0; x < cols2c; x++)
							for (k = 0; k < 31; k++)
							{
								features[j].index = j + 1;
								features[j].value = hptr[(cols - 1 - x) * 31 + _ccv_dpm_sym_lut[k]];
								++j;
							}
						hptr += hog->cols * 31;
					}
					features[j].index = j + 1;
					features[j].value = prob.bias;
					features[j + 1].index = -1;
					prob.x[l] = features;
					prob.y[l] = -1;
					++l;
				} else {
					features = (struct feature_node*)malloc(sizeof(struct feature_node) * (31 * cols * rows + 2));
					for (j = 0; j < 31 * rows * cols; j++)
					{
						features[j].index = j + 1;
						features[j].value = hog->data.f32[j];
					}
					features[31 * rows * cols].index = 31 * rows * cols + 1;
					features[31 * rows * cols].value = prob.bias;
					features[31 * rows * cols + 1].index = -1;
					prob.x[l] = features;
					prob.y[l] = -1;
					++l;
				}
				ccv_matrix_free(hog);
				++n;
				printf(".");
				fflush(stdout);
				if (n >= negnum)
					break;
			}
	}
	prob.l = l;
	printf("\n - generated %d examples with %d dimensions each\n"
		   " - running liblinear for initial linear SVM model (L2-regularized, L1-loss)\n", prob.l, prob.n);
	struct parameter linear_parameters = { .solver_type = L2R_L1LOSS_SVC_DUAL,
										   .eps = 1e-1,
										   .C = C,
										   .nr_weight = 0,
										   .weight_label = 0,
										   .weight = 0 };
	const char* err = check_parameter(&prob, &linear_parameters);
	if (err)
	{
		printf(" - ERROR: cannot pass check parameter: %s\n", err);
		exit(-1);
	}
	struct model* linear = train(&prob, &linear_parameters);
	assert(linear != 0);
	printf(" - model->label[0]: %d, model->nr_class: %d, model->nr_feature: %d\n", linear->label[0], linear->nr_class, linear->nr_feature);
	if (symmetric)
	{
		float* wptr = root_classifier->root.w->data.f32;
		for (y = 0; y < rows; y++)
		{
			for (x = 0; x < cols2c; x++)
				for (k = 0; k < 31; k++)
					wptr[(cols - 1 - x) * 31 + _ccv_dpm_sym_lut[k]] = wptr[x * 31 + k] = linear->w[(y * cols2c + x) * 31 + k];
			wptr += cols * 31;
		}
		// since for symmetric, lsvm only computed half features, to compensate that, we doubled the constant.
		root_classifier->beta = linear->w[31 * rows * cols2c] * 2.0;
	} else {
		for (j = 0; j < 31 * rows * cols; j++)
			root_classifier->root.w->data.f32[j] = linear->w[j];
		root_classifier->beta = linear->w[31 * rows * cols];
	}
	free_and_destroy_model(&linear);
	free(prob.y);
	for (j = 0; j < prob.l; j++)
		free(prob.x[j]);
	free(prob.x);
	ccv_make_matrix_immutable(root_classifier->root.w);
}

static void _ccv_dpm_initialize_part_classifiers(ccv_dpm_root_classifier_t* root_classifier, int parts, int symmetric)
{
	int i, j, k, x, y;
	ccv_dense_matrix_t* w = 0;
	ccv_sample_up(root_classifier->root.w, &w, 0, 0, 0);
	ccv_make_matrix_mutable(w);
	root_classifier->count = parts;
	root_classifier->part = (ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * parts);
	double area = w->rows * w->cols / (double)parts;
	for (i = 0; i < parts;)
	{
		ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + i;
		int dx = 0, dy = 0, dw = 0, dh = 0, sym = 0;
		double dsum = -1.0; // absolute value, thus, -1.0 is enough
#define slice_and_update_if_needed(y, x, l, n, s) \
		{ \
			ccv_dense_matrix_t* slice = 0; \
			ccv_slice(w, (ccv_matrix_t**)&slice, 0, y, x, l, n); \
			double sum = ccv_sum(slice, CCV_UNSIGNED) / (double)(l * n); \
			if (sum > dsum) \
			{ \
				dsum = sum; \
				dx = x; \
				dy = y; \
				dw = n; \
				dh = l; \
				sym = s; \
			} \
			ccv_matrix_free(slice); \
		}
		for (j = 1; (j < area + 1) && (j * 3 <= w->rows * 2); j++)
		{
			k = (int)(area / j + 0.5);
			if (k < 1 || k * 3 > w->cols * 2)
				continue;
			if (j > k * 2 || k > j * 2)
				continue;
			if (symmetric)
			{
				if (k % 2 == w->cols % 2) // can be symmetric in horizontal center
				{
					x = (w->cols - k) / 2;
					for (y = 0; y < w->rows - j + 1; y++)
						slice_and_update_if_needed(y, x, j, k, 0);
				}
				if (i < parts - 1) // have 2 locations
				{
					for (y = 0; y < w->rows - j + 1; y++)
						for (x = 0; x <= w->cols / 2 - k /* to avoid overlapping */; x++)
							slice_and_update_if_needed(y, x, j, k, 1);
				}
			} else {
				for (y = 0; y < w->rows - j + 1; y++)
					for (x = 0; x < w->cols - k + 1; x++)
						slice_and_update_if_needed(y, x, j, k, 0);
			}
		}
		printf(" ---- part %d(%d) %dx%d at (%d,%d), entropy: %lf\n", i + 1, parts, dw, dh, dx, dy, dsum);
		part_classifier->dx = 0;
		part_classifier->dy = 0;
		part_classifier->dxx = 0.1f;
		part_classifier->dyy = 0.1f;
		part_classifier->x = dx;
		part_classifier->y = dy;
		part_classifier->z = 1;
		part_classifier->w = 0;
		ccv_slice(w, (ccv_matrix_t**)&part_classifier->w, 0, dy, dx, dh, dw);
		ccv_make_matrix_immutable(part_classifier->w);
		/* clean up the region we selected */
		float* w_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | 31, w, dy, dx, 0);
		for (y = 0; y < dh; y++)
		{
			for (x = 0; x < dw * 31; x++)
				w_ptr[x] = 0;
			w_ptr += w->cols * 31;
		}
		i++;
		if (symmetric && sym) // add counter-part
		{
			dx = w->cols - (dx + dw);
			printf(" ---- part %d(%d) %dx%d at (%d,%d), entropy: %lf\n", i + 1, parts, dw, dh, dx, dy, dsum);
			part_classifier[1].dx = 0;
			part_classifier[1].dy = 0;
			part_classifier[1].dxx = 0.1f;
			part_classifier[1].dyy = 0.1f;
			part_classifier[1].x = dx;
			part_classifier[1].y = dy;
			part_classifier[1].z = 1;
			part_classifier[1].w = 0;
			ccv_slice(w, (ccv_matrix_t**)&part_classifier[1].w, 0, dy, dx, dh, dw);
			ccv_make_matrix_immutable(part_classifier[1].w);
			/* clean up the region we selected */
			float* w_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | 31, w, dy, dx, 0);
			for (y = 0; y < dh; y++)
			{
				for (x = 0; x < dw * 31; x++)
					w_ptr[x] = 0;
				w_ptr += w->cols * 31;
			}
			part_classifier[0].counterpart = i;
			part_classifier[1].counterpart = i - 1;
			i++;
		} else {
			part_classifier->counterpart = -1;
		}
	}
	ccv_matrix_free(w);
}

typedef struct {
	int id;
	int count;
	float score;
	ccv_dpm_part_classifier_t root;
	ccv_dpm_part_classifier_t* part;
} ccv_dpm_feature_vector_t;

static void _ccv_dpm_initialize_feature_vector_on_pattern(ccv_dpm_feature_vector_t* vector, ccv_dpm_root_classifier_t* root, int id)
{
	int i;
	vector->id = id;
	vector->count = root->count;
	vector->part = (ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * root->count);
	vector->root.w = ccv_dense_matrix_new(root->root.w->rows, root->root.w->cols, CCV_32F | 31, 0, 0);
	for (i = 0; i < vector->count; i++)
	{
		vector->part[i].x = root->part[i].x;
		vector->part[i].y = root->part[i].y;
		vector->part[i].z = root->part[i].z;
		vector->part[i].w = ccv_dense_matrix_new(root->part[i].w->rows, root->part[i].w->cols, CCV_32F | 31, 0, 0);
	}
}

static void _ccv_dpm_feature_vector_cleanup(ccv_dpm_feature_vector_t* vector)
{
	int i;
	ccv_matrix_free(vector->root.w);
	for (i = 0; i < vector->count; i++)
		ccv_matrix_free(vector->part[i].w);
	ccfree(vector->part);
}

static void _ccv_dpm_feature_vector_free(ccv_dpm_feature_vector_t* vector)
{
	_ccv_dpm_feature_vector_cleanup(vector);
	ccfree(vector);
}

static double _ccv_dpm_vector_score(ccv_dpm_mixture_model_t* model, ccv_dpm_feature_vector_t* v)
{
	if (v->id < 0 || v->id >= model->count)
		return 0;
	ccv_dpm_root_classifier_t* root_classifier = model->root + v->id;
	double score = root_classifier->beta;
	int i, k, ch = CCV_GET_CHANNEL(v->root.w->type);
	float *vptr = v->root.w->data.f32;
	float *wptr = root_classifier->root.w->data.f32;
	for (i = 0; i < v->root.w->rows * v->root.w->cols * ch; i++)
		score += wptr[i] * vptr[i];
	for (k = 0; k < v->count; k++)
	{
		ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + k;
		ccv_dpm_part_classifier_t* part_vector = v->part + k;
		score -= part_classifier->dx * part_vector->dx;
		score -= part_classifier->dxx * part_vector->dxx;
		score -= part_classifier->dy * part_vector->dy;
		score -= part_classifier->dyy * part_vector->dyy;
		vptr = part_vector->w->data.f32;
		wptr = part_classifier->w->data.f32;
		for (i = 0; i < part_vector->w->rows * part_vector->w->cols * ch; i++)
			score += wptr[i] * vptr[i];
	}
	return score;
}

static void _ccv_dpm_collect_feature_vector(ccv_dpm_feature_vector_t* v, float score, int x, int y, ccv_dense_matrix_t* pyr, ccv_dense_matrix_t* detail, ccv_dense_matrix_t** dx, ccv_dense_matrix_t** dy)
{
	v->score = score;
	ccv_zero(v->root.w);
	int rwh = (v->root.w->rows - 1) / 2, rww = (v->root.w->cols - 1) / 2;
	int i, ix, iy, ch = CCV_GET_CHANNEL(v->root.w->type);
	float* h_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | ch, pyr, y - rwh, x - rww, 0);
	float* w_ptr = v->root.w->data.f32;
	for (iy = 0; iy < v->root.w->rows; iy++)
	{
		memcpy(w_ptr, h_ptr, v->root.w->cols * ch * sizeof(float));
		h_ptr += pyr->cols * ch;
		w_ptr += v->root.w->cols * ch;
	}
	for (i = 0; i < v->count; i++)
	{
		ccv_dpm_part_classifier_t* part = v->part + i;
		int pww = (part->w->cols - 1) / 2, pwh = (part->w->rows - 1) / 2;
		int offy = part->y + pwh - rwh * 2;
		int offx = part->x + pww - rww * 2;
		iy = ccv_clamp(y * 2 + offy, pwh, detail->rows - part->w->rows + pwh);
		ix = ccv_clamp(x * 2 + offx, pww, detail->cols - part->w->cols + pww);
		int ry = ccv_get_dense_matrix_cell_value_by(CCV_32S | CCV_C1, dy[i], iy, ix, 0);
		int rx = ccv_get_dense_matrix_cell_value_by(CCV_32S | CCV_C1, dx[i], iy, ix, 0);
		part->dx = rx; // I am not sure if I need to flip the sign or not (confirmed, it should be this way)
		part->dy = ry;
		part->dxx = rx * rx;
		part->dyy = ry * ry;
		// deal with out-of-bound error
		int start_y = ccv_max(0, iy - ry - pwh);
		assert(start_y < detail->rows);
		int start_x = ccv_max(0, ix - rx - pww);
		assert(start_x < detail->cols);
		int end_y = ccv_min(detail->rows, iy - ry - pwh + part->w->rows);
		assert(end_y >= 0);
		int end_x = ccv_min(detail->cols, ix - rx - pww + part->w->cols);
		assert(end_x >= 0);
		h_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | ch, detail, start_y, start_x, 0);
		ccv_zero(v->part[i].w);
		w_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | ch, part->w, start_y - (iy - ry - pwh), start_x - (ix - rx - pww), 0);
		for (iy = start_y; iy < end_y; iy++)
		{
			memcpy(w_ptr, h_ptr, (end_x - start_x) * ch * sizeof(float));
			h_ptr += detail->cols * ch;
			w_ptr += part->w->cols * ch;
		}
	}
}

static ccv_dpm_feature_vector_t* _ccv_dpm_collect_best(ccv_dense_matrix_t* image, ccv_dpm_mixture_model_t* model, ccv_rect_t bbox, double overlap, ccv_dpm_param_t params)
{
	int i, j, k, x, y;
	double scale = pow(2.0, 1.0 / (params.interval + 1.0));
	int next = params.interval + 1;
	int scale_upto = _ccv_dpm_scale_upto(image, &model, 1, params.interval);
	if (scale_upto < 0)
		return 0;
	ccv_dense_matrix_t** pyr = (ccv_dense_matrix_t**)alloca((scale_upto + next * 2) * sizeof(ccv_dense_matrix_t*));
	_ccv_dpm_feature_pyramid(image, pyr, scale_upto, params.interval);
	float best = -FLT_MAX;
	ccv_dpm_feature_vector_t* v = 0;
	for (i = 0; i < model->count; i++)
	{
		ccv_dpm_root_classifier_t* root_classifier = model->root + i;
		double scale_x = 1.0;
		double scale_y = 1.0;
		for (j = next; j < scale_upto + next * 2; j++)
		{
			ccv_size_t size = ccv_size((int)(root_classifier->root.w->cols * CCV_DPM_WINDOW_SIZE * scale_x + 0.5), (int)(root_classifier->root.w->rows * CCV_DPM_WINDOW_SIZE * scale_y + 0.5));
			if ((double)(size.width * size.height) / (double)(bbox.width * bbox.height) < overlap || 
				(double)(bbox.width * bbox.height) / (double)(size.width * size.height) < overlap)
			{
				scale_x *= scale;
				scale_y *= scale;
				continue;
			}
			ccv_dense_matrix_t* root_feature = 0;
			ccv_dense_matrix_t* part_feature[CCV_DPM_PART_MAX];
			ccv_dense_matrix_t* dx[CCV_DPM_PART_MAX];
			ccv_dense_matrix_t* dy[CCV_DPM_PART_MAX];
			_ccv_dpm_compute_score(root_classifier, pyr[j], pyr[j - next], &root_feature, part_feature, dx, dy);
			int rwh = (root_classifier->root.w->rows - 1) / 2;
			int rww = (root_classifier->root.w->cols - 1) / 2;
			float* f_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | CCV_C1, root_feature, rwh, 0, 0);
			for (y = rwh; y < root_feature->rows - rwh; y++)
			{
				for (x = rww; x < root_feature->cols - rww; x++)
				{
					ccv_rect_t rect = ccv_rect((int)((x - rww) * CCV_DPM_WINDOW_SIZE * scale_x + 0.5), (int)((y - rwh) * CCV_DPM_WINDOW_SIZE * scale_y + 0.5), (int)(root_classifier->root.w->cols * CCV_DPM_WINDOW_SIZE * scale_x + 0.5), (int)(root_classifier->root.w->rows * CCV_DPM_WINDOW_SIZE * scale_y + 0.5));
					if ((double)(ccv_max(0, ccv_min(rect.x + rect.width, bbox.x + bbox.width) - ccv_max(rect.x, bbox.x)) *
								 ccv_max(0, ccv_min(rect.y + rect.height, bbox.y + bbox.height) - ccv_max(rect.y, bbox.y))) /
						(double)ccv_min(rect.width * rect.height, bbox.width * bbox.height) >= overlap && f_ptr[x] > best)
					{
						// initialize v
						if (v == 0)
						{
							v = (ccv_dpm_feature_vector_t*)ccmalloc(sizeof(ccv_dpm_feature_vector_t));
							_ccv_dpm_initialize_feature_vector_on_pattern(v, root_classifier, i);
						}
						// if it is another kind, cleanup and reinitialize
						if (v->id != i)
						{
							_ccv_dpm_feature_vector_cleanup(v);
							_ccv_dpm_initialize_feature_vector_on_pattern(v, root_classifier, i);
						}
						_ccv_dpm_collect_feature_vector(v, f_ptr[x] + root_classifier->beta, x, y, pyr[j], pyr[j - next], dx, dy);
						best = f_ptr[x];
					}
				}
				f_ptr += root_feature->cols;
			}
			for (k = 0; k < root_classifier->count; k++)
			{
				ccv_matrix_free(part_feature[k]);
				ccv_matrix_free(dx[k]);
				ccv_matrix_free(dy[k]);
			}
			ccv_matrix_free(root_feature);
			scale_x *= scale;
			scale_y *= scale;
		}
	}
	for (i = 0; i < scale_upto + next * 2; i++)
		ccv_matrix_free(pyr[i]);
	return v;
}

static ccv_array_t* _ccv_dpm_collect_all(gsl_rng* rng, ccv_dense_matrix_t* image, ccv_dpm_mixture_model_t* model, ccv_dpm_param_t params, float threshold)
{
	int i, j, k, x, y;
	double scale = pow(2.0, 1.0 / (params.interval + 1.0));
	int next = params.interval + 1;
	int scale_upto = _ccv_dpm_scale_upto(image, &model, 1, params.interval);
	if (scale_upto < 0)
		return 0;
	ccv_dense_matrix_t** pyr = (ccv_dense_matrix_t**)alloca((scale_upto + next * 2) * sizeof(ccv_dense_matrix_t*));
	_ccv_dpm_feature_pyramid(image, pyr, scale_upto, params.interval);
	ccv_array_t* av = ccv_array_new(64, sizeof(ccv_dpm_feature_vector_t*));
	int enough = 64 / model->count;
	for (i = 0; i < model->count; i++)
	{
		ccv_dpm_root_classifier_t* root_classifier = model->root + i;
		double scale_x = 1.0;
		double scale_y = 1.0;
		for (j = next; j < scale_upto + next * 2; j++)
		{
			ccv_dense_matrix_t* root_feature = 0;
			ccv_dense_matrix_t* part_feature[CCV_DPM_PART_MAX];
			ccv_dense_matrix_t* dx[CCV_DPM_PART_MAX];
			ccv_dense_matrix_t* dy[CCV_DPM_PART_MAX];
			_ccv_dpm_compute_score(root_classifier, pyr[j], pyr[j - next], &root_feature, part_feature, dx, dy);
			int rwh = (root_classifier->root.w->rows - 1) / 2;
			int rww = (root_classifier->root.w->cols - 1) / 2;
			float* f_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | CCV_C1, root_feature, rwh, 0, 0);
			for (y = rwh; y < root_feature->rows - rwh; y++)
			{
				for (x = rww; x < root_feature->cols - rww; x++)
					if (f_ptr[x] + root_classifier->beta > threshold)
					{
						// initialize v
						ccv_dpm_feature_vector_t* v = (ccv_dpm_feature_vector_t*)ccmalloc(sizeof(ccv_dpm_feature_vector_t));
						_ccv_dpm_initialize_feature_vector_on_pattern(v, root_classifier, i);
						_ccv_dpm_collect_feature_vector(v, f_ptr[x] + root_classifier->beta, x, y, pyr[j], pyr[j - next], dx, dy);
						ccv_array_push(av, &v);
						if (av->rnum >= enough * (i + 1))
							break;
					}
				f_ptr += root_feature->cols;
				if (av->rnum >= enough * (i + 1))
					break;
			}
			for (k = 0; k < root_classifier->count; k++)
			{
				ccv_matrix_free(part_feature[k]);
				ccv_matrix_free(dx[k]);
				ccv_matrix_free(dy[k]);
			}
			ccv_matrix_free(root_feature);
			scale_x *= scale;
			scale_y *= scale;
			if (av->rnum >= enough * (i + 1))
				break;
		}
	}
	for (i = 0; i < scale_upto + next * 2; i++)
		ccv_matrix_free(pyr[i]);
	return av;
}

#define NEGATIVE_CACHE_SIZE (500)

static void _ccv_dpm_collect_from_background(ccv_array_t* av, gsl_rng* rng, char** bgfiles, int bgnum, ccv_dpm_mixture_model_t* model, ccv_dpm_new_param_t params, float threshold)
{
	int i, j;
	int* order = (int*)ccmalloc(sizeof(int) * bgnum);
	for (i = 0; i < bgnum; i++)
		order[i] = i;
	gsl_ran_shuffle(rng, order, bgnum, sizeof(int));
	for (i = 0; i < bgnum; i++)
	{
		printf("\b\b\b\b\b\b%3d%%) ", av->rnum * 100 / NEGATIVE_CACHE_SIZE);
		fflush(stdout);
		ccv_dense_matrix_t* image = 0;
		ccv_read(bgfiles[order[i]], &image, (params.grayscale ? CCV_IO_GRAY : 0) | CCV_IO_ANY_FILE);
		ccv_array_t* at = _ccv_dpm_collect_all(rng, image, model, params.detector, threshold);
		if (at)
		{
			for (j = 0; j < at->rnum; j++)
				ccv_array_push(av, ccv_array_get(at, j));
			ccv_array_free(at);
		}
		ccv_matrix_free(image);
		if (av->rnum > NEGATIVE_CACHE_SIZE)
			break;
	}
	ccfree(order);
}

static void _ccv_dpm_initialize_root_rectangle_estimator(ccv_dpm_mixture_model_t* model, char** posfiles, ccv_rect_t* bboxes, int posnum, ccv_dpm_new_param_t params)
{
	int i;
	for (i = 0; i < posnum; i++)
	{
		ccv_dense_matrix_t* image = 0;
		ccv_read(posfiles[i], &image, (params.grayscale ? CCV_IO_GRAY : 0) | CCV_IO_ANY_FILE);
		ccv_dpm_feature_vector_t* v = _ccv_dpm_collect_best(image, model, bboxes[i], params.overlap, params.detector);
		ccv_matrix_free(image);
		_ccv_dpm_feature_vector_free(v);
	}
}

static void _ccv_dpm_regularize_mixture_model(ccv_dpm_mixture_model_t* model, double regz)
{
	int i, k;
	for (i = 0; i < model->count; i++)
	{
		ccv_dpm_root_classifier_t* root_classifier = model->root + i;
		int ch = CCV_GET_CHANNEL(root_classifier->root.w->type);
		ccv_make_matrix_mutable(root_classifier->root.w);
		float *wptr = root_classifier->root.w->data.f32;
		for (i = 0; i < root_classifier->root.w->rows * root_classifier->root.w->cols * ch; i++)
			wptr[i] -= regz * wptr[i];
		ccv_make_matrix_immutable(root_classifier->root.w);
		root_classifier->beta -= regz * root_classifier->beta;
		for (k = 0; k < root_classifier->count; k++)
		{
			ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + k;
			ccv_make_matrix_mutable(part_classifier->w);
			wptr = part_classifier->w->data.f32;
			for (i = 0; i < part_classifier->w->rows * part_classifier->w->cols * ch; i++)
				wptr[i] -= regz * wptr[i];
			ccv_make_matrix_immutable(part_classifier->w);
			part_classifier->dx -= regz * part_classifier->dx;
			part_classifier->dxx -= regz * part_classifier->dxx;
			part_classifier->dy -= regz * part_classifier->dy;
			part_classifier->dyy -= regz * part_classifier->dyy;
			part_classifier->dxx = ccv_max(0.01, part_classifier->dxx);
			part_classifier->dyy = ccv_max(0.01, part_classifier->dyy);
		}
	}
}

static void _ccv_dpm_stochastic_gradient_descent(ccv_dpm_mixture_model_t* model, ccv_dpm_feature_vector_t* v, double y, double alpha, double Cn, int symmetric)
{
	if (v->id < 0 || v->id >= model->count)
		return;
	ccv_dpm_root_classifier_t* root_classifier = model->root + v->id;
	int i, j, k, c, ch = CCV_GET_CHANNEL(v->root.w->type);
	float *vptr = v->root.w->data.f32;
	ccv_make_matrix_mutable(root_classifier->root.w);
	float *wptr = root_classifier->root.w->data.f32;
	if (symmetric)
	{
		for (i = 0; i < v->root.w->rows; i++)
		{
			for (j = 0; j < v->root.w->cols; j++)
				for (c = 0; c < ch; c++)
				{
					wptr[j * ch + c] += alpha * y * Cn * vptr[j * ch + c];
					wptr[j * ch + c] += alpha * y * Cn * vptr[(v->root.w->cols - 1 - j) * ch + _ccv_dpm_sym_lut[c]];
				}
			vptr += v->root.w->cols * ch;
			wptr += root_classifier->root.w->cols * ch;
		}
		root_classifier->beta += alpha * y * Cn * 2.0;
	} else {
		for (i = 0; i < v->root.w->rows * v->root.w->cols * ch; i++)
			wptr[i] += alpha * y * Cn * vptr[i];
		root_classifier->beta += alpha * y * Cn;
	}
	ccv_make_matrix_immutable(root_classifier->root.w);
	for (k = 0; k < v->count; k++)
	{
		ccv_dpm_part_classifier_t* part_classifier = root_classifier->part + k;
		ccv_make_matrix_mutable(part_classifier->w);
		ccv_dpm_part_classifier_t* part_vector = v->part + k;
		part_classifier->dx -= alpha * y * Cn * part_vector->dx;
		part_classifier->dxx -= alpha * y * Cn * part_vector->dxx;
		part_classifier->dxx = ccv_max(part_classifier->dxx, 0.01);
		part_classifier->dy -= alpha * y * Cn * part_vector->dy;
		part_classifier->dyy -= alpha * y * Cn * part_vector->dyy;
		part_classifier->dyy = ccv_max(part_classifier->dyy, 0.01);
		vptr = part_vector->w->data.f32;
		wptr = part_classifier->w->data.f32;
		if (symmetric)
		{
			// 2x converge on everything for symmetric feature
			if (part_classifier->counterpart == -1)
			{
				part_classifier->dx += /* flip the sign on x-axis (symmetric) */ alpha * y * Cn * part_vector->dx;
				part_classifier->dxx -= alpha * y * Cn * part_vector->dxx;
				part_classifier->dxx = ccv_max(part_classifier->dxx, 0.01);
				part_classifier->dy -= alpha * y * Cn * part_vector->dy;
				part_classifier->dyy -= alpha * y * Cn * part_vector->dyy;
				part_classifier->dyy = ccv_max(part_classifier->dyy, 0.01);
				for (i = 0; i < part_vector->w->rows; i++)
				{
					for (j = 0; j < part_vector->w->cols; j++)
						for (c = 0; c < ch; c++)
						{
							wptr[j * ch + c] += alpha * y * Cn * vptr[j * ch + c];
							wptr[j * ch + c] += alpha * y * Cn * vptr[(part_vector->w->cols - 1 - j) * ch + _ccv_dpm_sym_lut[c]];
						}
					vptr += part_vector->w->cols * ch;
					wptr += part_classifier->w->cols * ch;
				}
			} else {
				ccv_dpm_part_classifier_t* other_part_classifier = root_classifier->part + part_classifier->counterpart;
				other_part_classifier->dx += /* flip the sign on x-axis (symmetric) */ alpha * y * Cn * part_vector->dx;
				other_part_classifier->dxx -= alpha * y * Cn * part_vector->dxx;
				other_part_classifier->dxx = ccv_max(other_part_classifier->dxx, 0.01);
				other_part_classifier->dy -= alpha * y * Cn * part_vector->dy;
				other_part_classifier->dyy -= alpha * y * Cn * part_vector->dyy;
				other_part_classifier->dyy = ccv_max(other_part_classifier->dyy, 0.01);
				for (i = 0; i < part_vector->w->rows; i++)
				{
					for (j = 0; j < part_vector->w->cols * ch; j++)
						wptr[j] += alpha * y * Cn * vptr[j];
					vptr += part_vector->w->cols * ch;
					wptr += part_classifier->w->cols * ch;
				}
				vptr = part_vector->w->data.f32;
				wptr = other_part_classifier->w->data.f32;
				for (i = 0; i < part_vector->w->rows; i++)
				{
					for (j = 0; j < part_vector->w->cols; j++)
						for (c = 0; c < ch; c++)
							wptr[j * ch + c] += alpha * y * Cn * vptr[(part_vector->w->cols - 1 - j) * ch + _ccv_dpm_sym_lut[c]];
					vptr += part_vector->w->cols * ch;
					wptr += other_part_classifier->w->cols * ch;
				}
			}
		} else {
			for (i = 0; i < part_vector->w->rows * part_vector->w->cols * ch; i++)
				wptr[i] += alpha * y * Cn * vptr[i];
		}
		ccv_make_matrix_immutable(part_classifier->w);
	}
}

static void _ccv_dpm_write_gradient_descent_progress(int i, int j, int k, double previous_loss, double positive_loss, double negative_loss, double loss, int pos_prog, int neg_prog, int* order, int n, const char* dir)
{
	char swpfile[1024];
	sprintf(swpfile, "%s.swp", dir);
	FILE* w = fopen(swpfile, "w+");
	if (!w)
		return;
	fprintf(w, "%d %d %d %la %la %la %la %d %d\n", i, j, k, previous_loss, positive_loss, negative_loss, loss, pos_prog, neg_prog);
	int x;
	if (order != 0 && n > 0)
	{
		fprintf(w, "%d", order[0]);
		for (x = 1; x < n; x++)
			fprintf(w, " %d", order[x]);
		fprintf(w, "\n");
	}
	fclose(w);
	rename(swpfile, dir);
}

static void _ccv_dpm_read_gradient_descent_progress(int* i, int* j, int* k, double* previous_loss, double* positive_loss, double* negative_loss, double* loss, int* pos_prog, int* neg_prog, int* order, int n, const char* dir)
{
	FILE* r = fopen(dir, "r");
	if (!r)
		return;
	fscanf(r, "%d %d %d %lf %lf %lf %lf %d %d", i, j, k, previous_loss, positive_loss, negative_loss, loss, pos_prog, neg_prog);
	int x;
	for (x = 0; x < n; x++)
		if (1 != fscanf(r, "%d", order + x))
			break;
	fclose(r);
}

static void _ccv_dpm_write_feature_vectors(ccv_dpm_feature_vector_t** vs, int n, const char* dir)
{
	FILE* w = fopen(dir, "w+");
	if (!w)
		return;
	fprintf(w, "%d\n", n);
	int i, j, ch, x, y;
	for (i = 0; i < n; i++)
	{
		ccv_dpm_feature_vector_t* v = vs[i];
		if (v != 0)
		{
			fprintf(w, "%d %d %d\n", v->id, v->root.w->rows, v->root.w->cols);
			ch = CCV_GET_CHANNEL(v->root.w->type);
			for (y = 0; y < v->root.w->rows; y++)
			{
				for (x = 0; x < v->root.w->cols * ch; x++)
					fprintf(w, "%a ", v->root.w->data.f32[y * v->root.w->cols * ch + x]);
				fprintf(w, "\n");
			}
			fprintf(w, "%d %a\n", v->count, v->score);
			for (j = 0; j < v->count; j++)
			{
				ccv_dpm_part_classifier_t* part_classifier = v->part + j;
				fprintf(w, "%la %la %la %la\n", part_classifier->dx, part_classifier->dy, part_classifier->dxx, part_classifier->dyy);
				fprintf(w, "%d %d %d\n", part_classifier->x, part_classifier->y, part_classifier->z);
				fprintf(w, "%d %d\n", part_classifier->w->rows, part_classifier->w->cols);
				ch = CCV_GET_CHANNEL(part_classifier->w->type);
				for (y = 0; y < part_classifier->w->rows; y++)
				{
					for (x = 0; x < part_classifier->w->cols * ch; x++)
						fprintf(w, "%a ", part_classifier->w->data.f32[y * part_classifier->w->cols * ch + x]);
					fprintf(w, "\n");
				}
			}
		} else {
			fprintf(w, "0 0 0\n");
		}
	}
	fclose(w);
}

static int _ccv_dpm_read_feature_vectors(ccv_dpm_feature_vector_t** vs, int _n, const char* dir)
{
	FILE* r = fopen(dir, "r");
	if (!r)
		return -1;
	int n;
	fscanf(r, "%d", &n);
	assert(n == _n);
	int i, j, k;
	for (i = 0; i < n; i++)
	{
		int id, rows, cols;
		fscanf(r, "%d %d %d", &id, &rows, &cols);
		if (rows == 0 && cols == 0)
		{
			vs[i] = 0;
			continue;
		}
		ccv_dpm_feature_vector_t* v = vs[i] = (ccv_dpm_feature_vector_t*)ccmalloc(sizeof(ccv_dpm_feature_vector_t));
		v->id = id;
		v->root.w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, 0, 0);
		for (j = 0; j < rows * cols * 31; j++)
			fscanf(r, "%f", &v->root.w->data.f32[j]);
		fscanf(r, "%d %f", &v->count, &v->score);
		v->part = (ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * v->count);
		for (j = 0; j < v->count; j++)
		{
			ccv_dpm_part_classifier_t* part_classifier = v->part + j;
			fscanf(r, "%lf %lf %lf %lf", &part_classifier->dx, &part_classifier->dy, &part_classifier->dxx, &part_classifier->dyy);
			fscanf(r, "%d %d %d", &part_classifier->x, &part_classifier->y, &part_classifier->z);
			fscanf(r, "%d %d", &rows, &cols);
			part_classifier->w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, 0, 0);
			for (k = 0; k < rows * cols * 31; k++)
				fscanf(r, "%f", &part_classifier->w->data.f32[k]);
		}
	}
	fclose(r);
	return 0;
}

static void _ccv_dpm_check_params(ccv_dpm_new_param_t params)
{
	assert(params.components > 0);
	assert(params.parts > 0);
	assert(params.grayscale == 0 || params.grayscale == 1);
	assert(params.symmetric == 0 || params.symmetric == 1);
	assert(params.min_area > 100);
	assert(params.max_area > params.min_area);
	assert(params.iterations > 0);
	assert(params.relabels > 0);
	assert(params.overlap > 0.1);
	assert(params.alpha > 0 && params.alpha < 1);
	assert(params.alpha_ratio > 0 && params.alpha_ratio < 1);
	assert(params.C > 0);
	assert(params.balance > 0);
	assert(params.percentile_breakdown > 0 && params.percentile_breakdown <= 1);
	assert(params.detector.interval > 0);
}

#define MINI_BATCH (10)
#define REGQ (100)

void ccv_dpm_mixture_model_new(char** posfiles, ccv_rect_t* bboxes, int posnum, char** bgfiles, int bgnum, int negnum, const char* dir, ccv_dpm_new_param_t params)
{
	int t, c, r, i, j, k;
	_ccv_dpm_check_params(params);
	gsl_rng_env_setup();
	gsl_rng* rng = gsl_rng_alloc(gsl_rng_default);
	gsl_rng_set(rng, *(unsigned long int*)&params);
	ccv_dpm_mixture_model_t* model = (ccv_dpm_mixture_model_t*)ccmalloc(sizeof(ccv_dpm_mixture_model_t));
	memset(model, 0, sizeof(ccv_dpm_mixture_model_t));
	struct feature_node* fn = (struct feature_node*)ccmalloc(sizeof(struct feature_node) * posnum);
	for (i = 0; i < posnum; i++)
	{
		assert(bboxes[i].width > 0 && bboxes[i].height > 0);
		fn[i].value = (float)bboxes[i].width / (float)bboxes[i].height;
		fn[i].index = i;
	}
	char checkpoint[512];
	char initcheckpoint[512];
	char subcheckpoint[512];
	sprintf(checkpoint, "%s/model", dir);
	sprintf(initcheckpoint, "%s/init.model", dir);
	sprintf(subcheckpoint, "%s/_model", dir);
	_ccv_dpm_aspect_qsort(fn, posnum, 0);
	double mean = 0;
	for (i = 0; i < posnum; i++)
		mean += fn[i].value;
	mean /= posnum;
	double variance = 0;
	for (i = 0; i < posnum; i++)
		variance = (fn[i].value - mean) * (fn[i].value - mean);
	variance /= posnum;
	printf("global mean: %lf, & variance: %lf\ninterclass mean(variance):", mean, variance);
	int* mnum = (int*)alloca(sizeof(int) * params.components);
	int outnum = posnum, innum = 0;
	for (i = 0; i < params.components; i++)
	{
		mnum[i] = (int)((double)outnum / (double)(params.components - i) + 0.5);
		double mean = 0;
		for (j = innum; j < innum + mnum[i]; j++)
			mean += fn[j].value;
		mean /= mnum[i];
		double variance = 0;
		for (j = innum; j < innum + mnum[i]; j++)
			variance = (fn[j].value - mean) * (fn[j].value - mean);
		variance /= mnum[i];
		printf(" %lf(%lf)", mean, variance);
		outnum -= mnum[i];
		innum += mnum[i];
	}
	printf("\n");
	int* areas = (int*)ccmalloc(sizeof(int) * posnum);
	for (i = 0; i < posnum; i++)
		areas[i] = bboxes[i].width * bboxes[i].height;
	_ccv_dpm_area_qsort(areas, posnum, 0);
	// so even the object is 1/4 in size, we can still detect them (in detection phase, we start at 2x image)
	int area = ccv_clamp(areas[(int)(posnum * 0.2 + 0.5)], params.min_area, params.max_area);
	ccfree(areas);
	innum = 0;
	_ccv_dpm_read_checkpoint(model, checkpoint);
	if (model->count <= 0)
	{
		/* initialize root mixture model with liblinear */
		model->count = params.components;
		model->root = (ccv_dpm_root_classifier_t*)ccmalloc(sizeof(ccv_dpm_root_classifier_t) * model->count);
		memset(model->root, 0, sizeof(ccv_dpm_root_classifier_t) * model->count);
	}
	printf("computing root mixture model dimensions: ");
	fflush(stdout);
	int* labels = (int*)ccmalloc(sizeof(int) * posnum);
	int* rows = (int*)alloca(sizeof(int) * params.components);
	int* cols = (int*)alloca(sizeof(int) * params.components);
	for (i = 0; i < params.components; i++)
	{
		double aspect = 0;
		for (j = innum; j < innum + mnum[i]; j++)
		{
			aspect += fn[j].value;
			labels[fn[j].index] = i; // setup labels
		}
		aspect /= mnum[i];
		cols[i] = ccv_max((int)(sqrtf(area / aspect) * aspect / CCV_DPM_WINDOW_SIZE + 0.5), 1);
		rows[i] = ccv_max((int)(sqrtf(area / aspect) / CCV_DPM_WINDOW_SIZE + 0.5), 1);
		if (i < params.components - 1)
			printf("%dx%d, ", cols[i], rows[i]);
		else
			printf("%dx%d\n", cols[i], rows[i]);
		fflush(stdout);
		innum += mnum[i];
	}
	for (i = 0; i < params.components; i++)
	{
		if (model->root[i].root.w != 0)
		{
			printf("skipping root mixture model initialization for model %d(%d)\n", i + 1, params.components);
			continue;
		}
		ccv_dpm_root_classifier_t* root_classifier = model->root + i;
		root_classifier->root.w = ccv_dense_matrix_new(rows[i], cols[i], CCV_32F | 31, 0, 0);
		printf("initializing root mixture model for model %d(%d)\n", i + 1, params.components);
		_ccv_dpm_initialize_root_classifier(rng, root_classifier, i, mnum[i], labels, posfiles, bboxes, posnum, bgfiles, bgnum, negnum, params.C, params.symmetric, params.grayscale);
		_ccv_dpm_write_checkpoint(model, checkpoint);
	}
	ccfree(fn);
	ccfree(labels);
	// check symmetric property of generated root feature
	if (params.symmetric)
		for (i = 0; i < params.components; i++)
		{
			ccv_dpm_root_classifier_t* root_classifier = model->root + i;
			_ccv_dpm_check_root_classifier_symmetry(root_classifier->root.w);
		}
	if (params.components > 1)
	{
		/* TODO: coordinate-descent for lsvm */
		printf("optimizing root mixture model with coordinate-descent approach\n");
	} else {
		printf("components == 1, skipped coordinate-descent to optimize root mixture model\n");
	}
	/* initialize part filter */
	printf("initializing part filters\n");
	for (i = 0; i < params.components; i++)
	{
		if (model->root[i].count > 0)
		{
			printf(" - skipping part filters initialization for model %d(%d)\n", i + 1, params.components);
		} else {
			printf(" - initializing part filters for model %d(%d)\n", i + 1, params.components);
			_ccv_dpm_initialize_part_classifiers(model->root + i, params.parts, params.symmetric);
			_ccv_dpm_write_checkpoint(model, checkpoint);
			_ccv_dpm_write_checkpoint(model, initcheckpoint);
		}
	}
	_ccv_dpm_write_checkpoint(model, checkpoint);
	/* optimize both root filter and part filters with stochastic gradient descent */
	printf("optimizing root filter & part filters with stochastic gradient descent\n");
	char gradient_progress_checkpoint[512];
	sprintf(gradient_progress_checkpoint, "%s/gradient_descent_progress", dir);
	char feature_vector_checkpoint[512];
	sprintf(feature_vector_checkpoint, "%s/positive_vectors", dir);
	ccv_dpm_feature_vector_t** posv = (ccv_dpm_feature_vector_t**)ccmalloc(posnum * sizeof(ccv_dpm_feature_vector_t*));
	int* order = (int*)ccmalloc(sizeof(int) * (posnum + negnum));
	int pos_prog = 0, neg_prog = 0;
	double previous_loss = 0, positive_loss = 0, negative_loss = 0, loss = 0;
	// need to re-weight for each examples
	double pos_weight = sqrt((double)negnum / posnum * params.balance); // positive weight
	double neg_weight = sqrt((double)posnum / negnum / params.balance); // negative weight
	c = t = r = 0;
	_ccv_dpm_read_gradient_descent_progress(&c, &t, &r, &previous_loss, &positive_loss, &negative_loss, &loss, &pos_prog, &neg_prog, order, posnum + negnum, gradient_progress_checkpoint);
	ccv_array_t* negv = 0;
	int negi = 0;
	int negk[NEGATIVE_CACHE_SIZE];
	for (i = 0; i < NEGATIVE_CACHE_SIZE; i++)
		negk[i] = i;
	for (; c < params.relabels; c++)
	{
		double regz_rate = params.C / model->count;
		double alpha = params.alpha * pow(params.alpha_ratio, t);
		ccv_dpm_mixture_model_t* _model;
		if (0 == _ccv_dpm_read_feature_vectors(posv, posnum, feature_vector_checkpoint))
		{
			printf(" - read collected positive responses from last interrupted process\n");
		} else {
			printf(" - collecting responses from positive examples :  0%%");
			fflush(stdout);
			for (i = 0; i < posnum; i++)
			{
				printf("\b\b\b%2d%%", i * 100 / posnum);
				fflush(stdout);
				ccv_dense_matrix_t* image = 0;
				ccv_read(posfiles[i], &image, (params.grayscale ? CCV_IO_GRAY : 0) | CCV_IO_ANY_FILE);
				posv[i] = _ccv_dpm_collect_best(image, model, bboxes[i], params.overlap, params.detector);
				ccv_matrix_free(image);
			}
			printf("\b\b\b100%%\n");
			_ccv_dpm_write_feature_vectors(posv, posnum, feature_vector_checkpoint);
		}
		params.detector.threshold = 0;
		for (; t < params.iterations; t++)
		{
			unsigned int elapsed_time = _ccv_dpm_time_measure();
			if (r == 0)
			{
				_model = _ccv_dpm_model_copy(model);
				printf(" - shuffling stochastic gradient descent order\n");
				for (i = 0; i < posnum + negnum; i++)
					order[i] = i;
				gsl_ran_shuffle(rng, order, posnum + negnum, sizeof(int));
				loss = 0;
			} else {
				_model = (ccv_dpm_mixture_model_t*)ccmalloc(sizeof(ccv_dpm_mixture_model_t));
				memset(_model, 0, sizeof(ccv_dpm_mixture_model_t));
				_ccv_dpm_read_checkpoint(_model, subcheckpoint);
			}
			_ccv_dpm_write_gradient_descent_progress(c, t, 0, previous_loss, positive_loss, negative_loss, loss, pos_prog, neg_prog, order, posnum + negnum, gradient_progress_checkpoint);
			printf(" - updating using stochastic gradient descent\n");
			int last_update = 0;
			for (i = r; i < posnum + negnum; i++)
			{
				k = order[i];
				if (k < posnum)
				{
					if (posv[k] != 0)
					{
						double score = _ccv_dpm_vector_score(model, posv[k]); // the loss for mini-batch method (computed on model)
						if (score <= 1)
							_ccv_dpm_stochastic_gradient_descent(_model, posv[k], 1, alpha * pos_weight, regz_rate, params.symmetric);
						double hinge_loss = ccv_max(0, 1.0 - score);
						positive_loss += hinge_loss;
						loss += pos_weight * hinge_loss;
						++pos_prog;
						for (j = 0; j < last_update; j++)
							printf("\b");
						last_update = printf(" - with loss %.5lf (positive %.5lf, negative %.5f) at rate %.5lf -+- %2d%%                                      ", loss / (pos_prog + neg_prog), positive_loss / pos_prog, negative_loss / neg_prog, alpha, (i + 1) * 100 / (posnum + negnum));
						fflush(stdout);
					}
				} else {
					// the cache is used up now, collect again
					if ((neg_prog % NEGATIVE_CACHE_SIZE) == 0)
					{
						if (negv)
						{
							ccv_array_t* av = ccv_array_new(64, sizeof(ccv_dpm_feature_vector_t*));
							for (j = 0; j < negv->rnum; j++)
							{
								ccv_dpm_feature_vector_t* v = *(ccv_dpm_feature_vector_t**)ccv_array_get(negv, j);
								double score = _ccv_dpm_vector_score(model, v);
								if (score >= -1)
									ccv_array_push(av, &v);
								else
									_ccv_dpm_feature_vector_free(v);
							}
							ccv_array_free(negv);
							negv = av;
						} else {
							negv = ccv_array_new(64, sizeof(ccv_dpm_feature_vector_t*));
						}
						for (j = 0; j < last_update; j++)
							printf("\b");
						last_update = printf(" - pause, collecting negative examples, current loss %.5lf (positive %.5lf, negative %.5f) --- %2d%% (  0%%) ", loss / ccv_max(1, pos_prog + neg_prog), positive_loss / ccv_max(1, pos_prog), negative_loss / ccv_max(1, neg_prog), (i + 1) * 100 / (posnum + negnum));
						fflush(stdout);
						if (negv->rnum < NEGATIVE_CACHE_SIZE)
							_ccv_dpm_collect_from_background(negv, rng, bgfiles, bgnum, model, params, 0);
						negi = 0;
						gsl_ran_shuffle(rng, negk, NEGATIVE_CACHE_SIZE, sizeof(int));
					}
					if (negv && negv->rnum > 0)
					{
						k = negk[negi];
						++negi;
						ccv_dpm_feature_vector_t* v = *(ccv_dpm_feature_vector_t**)ccv_array_get(negv, k % negv->rnum);
						double score = _ccv_dpm_vector_score(model, v);
						if (score >= -1)
							_ccv_dpm_stochastic_gradient_descent(_model, v, -1, alpha * neg_weight, regz_rate, params.symmetric);
						double hinge_loss = ccv_max(0, 1.0 + score);
						negative_loss += hinge_loss;
						loss += neg_weight * hinge_loss;
						++neg_prog;
						for (j = 0; j < last_update; j++)
							printf("\b");
						last_update = printf(" - with loss %.5lf (positive %.5lf, negative %.5f) at rate %.5lf --- %2d%%                                      ", loss / (pos_prog + neg_prog), positive_loss / pos_prog, negative_loss / neg_prog, alpha, (i + 1) * 100 / (posnum + negnum));
						fflush(stdout);
					}
				}
				if (i % REGQ == REGQ - 1)
				{
					for (j = 0; j < last_update; j++)
						printf("\b");
					last_update = printf(" - with loss %.5lf (positive %.5lf, negative %.5f) at rate %.5lf -o- %2d%%                                      ", loss / (pos_prog + neg_prog), positive_loss / pos_prog, negative_loss / neg_prog, alpha, (i + 1) * 100 / (posnum + negnum));
					fflush(stdout);
					_ccv_dpm_regularize_mixture_model(_model, 1.0 - pow(1.0 - alpha / (double)((posnum + negnum) * (!!params.symmetric + 1)), REGQ));
					_ccv_dpm_write_gradient_descent_progress(c, t, i, previous_loss, positive_loss, negative_loss, loss, pos_prog, neg_prog, order, posnum + negnum, gradient_progress_checkpoint);
					_ccv_dpm_write_checkpoint(_model, subcheckpoint);
					_ccv_dpm_write_checkpoint(model, checkpoint);
				}
				if (i % MINI_BATCH == MINI_BATCH - 1)
				{
					// mimicking mini-batch way of doing things
					_ccv_dpm_mixture_model_cleanup(model);
					ccfree(model);
					model = _model;
					_model = _ccv_dpm_model_copy(model);
				}
			}
			printf("\n");
			if (negv)
			{
				ccv_array_t* av = ccv_array_new(64, sizeof(ccv_dpm_feature_vector_t*));
				for (j = 0; j < negv->rnum; j++)
				{
					ccv_dpm_feature_vector_t* v = *(ccv_dpm_feature_vector_t**)ccv_array_get(negv, j);
					double score = _ccv_dpm_vector_score(model, v);
					if (score >= -1)
						ccv_array_push(av, &v);
					else
						_ccv_dpm_feature_vector_free(v);
				}
				ccv_array_free(negv);
				negv = av;
			}
			_ccv_dpm_regularize_mixture_model(_model, 1.0 - pow(1.0 - alpha * (((posnum + negnum) % REGQ) + 1) / (double)((posnum + negnum) * (!!params.symmetric + 1)), ((posnum + negnum) % REGQ) + 1));
			_ccv_dpm_mixture_model_cleanup(model);
			ccfree(model);
			model = _model;
			_ccv_dpm_write_checkpoint(model, checkpoint);
			// check symmetric property of generated root feature
			if (params.symmetric)
				for (i = 0; i < params.components; i++)
				{
					ccv_dpm_root_classifier_t* root_classifier = model->root + i;
					_ccv_dpm_check_root_classifier_symmetry(root_classifier->root.w);
				}
			printf(" - %d iteration takes %.2lf seconds, with %d positive reactions and %d negative reactions at loss %.5lf, %d more to go (%d of %d)\n", t + 1, (double)(_ccv_dpm_time_measure() - elapsed_time) / 1000.0, pos_prog, neg_prog, loss / (pos_prog + neg_prog), params.iterations - t - 1, c + 1, params.relabels);
			j = 0;
			double* scores = (double*)ccmalloc(posnum * sizeof(double));
			for (i = 0; i < posnum; i++)
				if (posv[i] != 0)
				{
					scores[j] = _ccv_dpm_vector_score(model, posv[i]);
					j++;
				}
			_ccv_dpm_score_qsort(scores, j, 0);
			ccfree(scores);
			double breakdown;
			printf(" - threshold breakdown by percentile");
			for (breakdown = params.percentile_breakdown; breakdown < 1.0; breakdown += params.percentile_breakdown)
				printf(" %0.2lf(%.1f%%)", scores[ccv_clamp((int)(breakdown * j), 0, j - 1)], (1.0 - breakdown) * 100);
			printf("\n");
			alpha *= params.alpha_ratio; // it will decrease with each iteration
			pos_prog = neg_prog = r = 0;
			char persist[512];
			sprintf(persist, "%s/model.%d.%d", dir, c, t);
			_ccv_dpm_write_checkpoint(model, persist);
			if (fabs(previous_loss - loss) < 1e-4)
			{
				printf(" - aborting iteration at %d because we didn't gain much\n", t + 1);
				break;
			}
			previous_loss = loss;
			positive_loss = negative_loss = loss = 0;
		}
		if (negv)
		{
			for (j = 0; j < negv->rnum; j++)
			{
				ccv_dpm_feature_vector_t* v = *(ccv_dpm_feature_vector_t**)ccv_array_get(negv, j);
				_ccv_dpm_feature_vector_free(v);
			}
			ccv_array_free(negv);
			negv = 0;
			negi = 0;
			gsl_ran_shuffle(rng, negk, NEGATIVE_CACHE_SIZE, sizeof(int));
		}
		t = 0;
		previous_loss = positive_loss = negative_loss = loss = 0;
		for (i = 0; i < posnum; i++)
			if (posv[i] != 0)
				_ccv_dpm_feature_vector_free(posv[i]);
		remove(feature_vector_checkpoint);
	}
	remove(gradient_progress_checkpoint);
	ccfree(order);
	ccfree(posv);
	_ccv_dpm_mixture_model_cleanup(model);
	ccfree(model);
	gsl_rng_free(rng);
}

static int _ccv_is_equal(const void* _r1, const void* _r2, void* data)
{
	const ccv_root_comp_t* r1 = (const ccv_root_comp_t*)_r1;
	const ccv_root_comp_t* r2 = (const ccv_root_comp_t*)_r2;
	int distance = (int)(ccv_min(r1->rect.width, r1->rect.height) * 0.25 + 0.5);

	return r2->rect.x <= r1->rect.x + distance &&
		r2->rect.x >= r1->rect.x - distance &&
		r2->rect.y <= r1->rect.y + distance &&
		r2->rect.y >= r1->rect.y - distance &&
		r2->rect.width <= (int)(r1->rect.width * 1.5 + 0.5) &&
		(int)(r2->rect.width * 1.5 + 0.5) >= r1->rect.width &&
		r2->rect.height <= (int)(r1->rect.height * 1.5 + 0.5) &&
		(int)(r2->rect.height * 1.5 + 0.5) >= r1->rect.height;
}

static int _ccv_is_equal_same_class(const void* _r1, const void* _r2, void* data)
{
	const ccv_root_comp_t* r1 = (const ccv_root_comp_t*)_r1;
	const ccv_root_comp_t* r2 = (const ccv_root_comp_t*)_r2;
	int distance = (int)(ccv_min(r1->rect.width, r1->rect.height) * 0.25 + 0.5);

	return r2->id == r1->id &&
		r2->rect.x <= r1->rect.x + distance &&
		r2->rect.x >= r1->rect.x - distance &&
		r2->rect.y <= r1->rect.y + distance &&
		r2->rect.y >= r1->rect.y - distance &&
		r2->rect.width <= (int)(r1->rect.width * 1.5 + 0.5) &&
		(int)(r2->rect.width * 1.5 + 0.5) >= r1->rect.width &&
		r2->rect.height <= (int)(r1->rect.height * 1.5 + 0.5) &&
		(int)(r2->rect.height * 1.5 + 0.5) >= r1->rect.height;
}

ccv_array_t* ccv_dpm_detect_objects(ccv_dense_matrix_t* a, ccv_dpm_mixture_model_t** _model, int count, ccv_dpm_param_t params)
{
	int c, i, j, k, x, y;
	double scale = pow(2.0, 1.0 / (params.interval + 1.0));
	int next = params.interval + 1;
	int scale_upto = _ccv_dpm_scale_upto(a, _model, count, params.interval);
	if (scale_upto < 0) // image is too small to be interesting
		return 0;
	ccv_dense_matrix_t** pyr = (ccv_dense_matrix_t**)alloca((scale_upto + next * 2) * sizeof(ccv_dense_matrix_t*));
	_ccv_dpm_feature_pyramid(a, pyr, scale_upto, params.interval);
	ccv_array_t* idx_seq;
	ccv_array_t* seq = ccv_array_new(64, sizeof(ccv_root_comp_t));
	ccv_array_t* seq2 = ccv_array_new(64, sizeof(ccv_root_comp_t));
	ccv_array_t* result_seq = ccv_array_new(64, sizeof(ccv_root_comp_t));
	for (c = 0; c < count; c++)
	{
		ccv_dpm_mixture_model_t* model = _model[c];
		double scale_x = 1.0;
		double scale_y = 1.0;
		for (i = next; i < scale_upto + next * 2; i++)
		{
			for (j = 0; j < model->count; j++)
			{
				ccv_dpm_root_classifier_t* root = model->root + j;
				ccv_dense_matrix_t* root_feature = 0;
				ccv_dense_matrix_t* part_feature[CCV_DPM_PART_MAX];
				ccv_dense_matrix_t* dx[CCV_DPM_PART_MAX];
				ccv_dense_matrix_t* dy[CCV_DPM_PART_MAX];
				_ccv_dpm_compute_score(root, pyr[i], pyr[i - next], &root_feature, part_feature, dx, dy);
				int rwh = (root->root.w->rows - 1) / 2;
				int rww = (root->root.w->cols - 1) / 2;
				float* f_ptr = (float*)ccv_get_dense_matrix_cell_by(CCV_32F | CCV_C1, root_feature, rwh, 0, 0);
				for (y = rwh; y < root_feature->rows - rwh; y++)
				{
					for (x = rww; x < root_feature->cols - rww; x++)
						if (f_ptr[x] + root->beta > params.threshold)
						{
							ccv_root_comp_t comp;
							comp.rect = ccv_rect((int)((x - rww) * CCV_DPM_WINDOW_SIZE * scale_x + 0.5), (int)((y - rwh) * CCV_DPM_WINDOW_SIZE * scale_y + 0.5), (int)(root->root.w->cols * CCV_DPM_WINDOW_SIZE * scale_x + 0.5), (int)(root->root.w->rows * CCV_DPM_WINDOW_SIZE * scale_y + 0.5));
							comp.id = c;
							comp.neighbors = 1;
							comp.confidence = f_ptr[x] + root->beta;
							comp.pnum = root->count;
							for (k = 0; k < root->count; k++)
							{
								ccv_dpm_part_classifier_t* part = root->part + k;
								comp.part[k].id = c;
								comp.part[k].neighbors = 1;
								int pww = (part->w->cols - 1) / 2, pwh = (part->w->rows - 1) / 2;
								int offy = part->y + pwh - rwh * 2;
								int offx = part->x + pww - rww * 2;
								int iy = ccv_clamp(y * 2 + offy, pwh, part_feature[k]->rows - pwh);
								int ix = ccv_clamp(x * 2 + offx, pww, part_feature[k]->cols - pww);
								int ry = iy - ccv_get_dense_matrix_cell_value_by(CCV_32S | CCV_C1, dy[k], iy, ix, 0);
								int rx = ix - ccv_get_dense_matrix_cell_value_by(CCV_32S | CCV_C1, dx[k], iy, ix, 0);
								comp.part[k].rect = ccv_rect((int)((rx - pww) * CCV_DPM_WINDOW_SIZE / 2 * scale_x + 0.5), (int)((ry - pwh) * CCV_DPM_WINDOW_SIZE / 2 * scale_y + 0.5), (int)(part->w->cols * CCV_DPM_WINDOW_SIZE / 2 * scale_x + 0.5), (int)(part->w->rows * CCV_DPM_WINDOW_SIZE / 2 * scale_y + 0.5));
								comp.part[k].confidence = -ccv_get_dense_matrix_cell_value_by(CCV_32F | CCV_C1, part_feature[k], iy, ix, 0);
							}
							ccv_array_push(seq, &comp);
						}
					f_ptr += root_feature->cols;
				}
				for (k = 0; k < root->count; k++)
				{
					ccv_matrix_free(part_feature[k]);
					ccv_matrix_free(dx[k]);
					ccv_matrix_free(dy[k]);
				}
				ccv_matrix_free(root_feature);
			}
			scale_x *= scale;
			scale_y *= scale;
		}
		/* the following code from OpenCV's haar feature implementation */
		if (params.min_neighbors == 0)
		{
			for (i = 0; i < seq->rnum; i++)
			{
				ccv_root_comp_t* comp = (ccv_root_comp_t*)ccv_array_get(seq, i);
				ccv_array_push(result_seq, comp);
			}
		} else {
			idx_seq = 0;
			ccv_array_clear(seq2);
			// group retrieved rectangles in order to filter out noise
			int ncomp = ccv_array_group(seq, &idx_seq, _ccv_is_equal_same_class, 0);
			ccv_root_comp_t* comps = (ccv_root_comp_t*)ccmalloc((ncomp + 1) * sizeof(ccv_root_comp_t));
			memset(comps, 0, (ncomp + 1) * sizeof(ccv_root_comp_t));

			// count number of neighbors
			for(i = 0; i < seq->rnum; i++)
			{
				ccv_root_comp_t r1 = *(ccv_root_comp_t*)ccv_array_get(seq, i);
				int idx = *(int*)ccv_array_get(idx_seq, i);

				comps[idx].id = r1.id;
				comps[idx].pnum = r1.pnum;
				if (r1.confidence > comps[idx].confidence || comps[idx].neighbors == 0)
				{
					comps[idx].rect = r1.rect;
					comps[idx].confidence = r1.confidence;
					memcpy(comps[idx].part, r1.part, sizeof(ccv_comp_t) * CCV_DPM_PART_MAX);
				}

				++comps[idx].neighbors;
			}

			// calculate average bounding box
			for(i = 0; i < ncomp; i++)
			{
				int n = comps[i].neighbors;
				if(n >= params.min_neighbors)
					ccv_array_push(seq2, comps + i);
			}

			// filter out small face rectangles inside large face rectangles
			for(i = 0; i < seq2->rnum; i++)
			{
				ccv_root_comp_t r1 = *(ccv_root_comp_t*)ccv_array_get(seq2, i);
				int flag = 1;

				for(j = 0; j < seq2->rnum; j++)
				{
					ccv_root_comp_t r2 = *(ccv_root_comp_t*)ccv_array_get(seq2, j);
					int distance = (int)(r2.rect.width * 0.25 + 0.5);

					if(i != j &&
					   r1.id == r2.id &&
					   r1.rect.x >= r2.rect.x - distance &&
					   r1.rect.y >= r2.rect.y - distance &&
					   r1.rect.x + r1.rect.width <= r2.rect.x + r2.rect.width + distance &&
					   r1.rect.y + r1.rect.height <= r2.rect.y + r2.rect.height + distance &&
					   (r2.neighbors > ccv_max(3, r1.neighbors) || r1.neighbors < 3))
					{
						flag = 0;
						break;
					}
				}

				if(flag)
					ccv_array_push(result_seq, &r1);
			}
			ccv_array_free(idx_seq);
			ccfree(comps);
		}
	}

	for (i = 0; i < scale_upto + next * 2; i++)
		ccv_matrix_free(pyr[i]);

	ccv_array_free(seq);
	ccv_array_free(seq2);

	ccv_array_t* result_seq2;
	/* the following code from OpenCV's haar feature implementation */
	if (params.flags & CCV_DPM_NO_NESTED)
	{
		result_seq2 = ccv_array_new(64, sizeof(ccv_root_comp_t));
		idx_seq = 0;
		// group retrieved rectangles in order to filter out noise
		int ncomp = ccv_array_group(result_seq, &idx_seq, _ccv_is_equal, 0);
		ccv_root_comp_t* comps = (ccv_root_comp_t*)ccmalloc((ncomp + 1) * sizeof(ccv_root_comp_t));
		memset(comps, 0, (ncomp + 1) * sizeof(ccv_root_comp_t));

		// count number of neighbors
		for(i = 0; i < result_seq->rnum; i++)
		{
			ccv_root_comp_t r1 = *(ccv_root_comp_t*)ccv_array_get(result_seq, i);
			int idx = *(int*)ccv_array_get(idx_seq, i);

			if (comps[idx].neighbors == 0 || comps[idx].confidence < r1.confidence)
			{
				comps[idx].confidence = r1.confidence;
				comps[idx].neighbors = 1;
				comps[idx].rect = r1.rect;
				comps[idx].id = r1.id;
				comps[idx].pnum = r1.pnum;
				memcpy(comps[idx].part, r1.part, sizeof(ccv_comp_t) * CCV_DPM_PART_MAX);
			}
		}

		// calculate average bounding box
		for(i = 0; i < ncomp; i++)
			if(comps[i].neighbors)
				ccv_array_push(result_seq2, &comps[i]);

		ccv_array_free(result_seq);
		ccfree(comps);
	} else {
		result_seq2 = result_seq;
	}

	return result_seq2;
}

ccv_dpm_mixture_model_t* ccv_load_dpm_mixture_model(const char* directory)
{
	FILE* r = fopen(directory, "r");
	if (r == 0)
		return 0;
	int count;
	char flag;
	fscanf(r, "%c", &flag);
	assert(flag == '.');
	fscanf(r, "%d", &count);
	ccv_dpm_root_classifier_t* root_classifier = (ccv_dpm_root_classifier_t*)ccmalloc(sizeof(ccv_dpm_root_classifier_t) * count);
	memset(root_classifier, 0, sizeof(ccv_dpm_root_classifier_t) * count);
	int i, j, k;
	size_t size = sizeof(ccv_dpm_mixture_model_t) + sizeof(ccv_dpm_root_classifier_t) * count;
	/* the format is easy, but I tried to copy all data into one memory region */
	for (i = 0; i < count; i++)
	{
		int rows, cols;
		fscanf(r, "%d %d", &rows, &cols);
		fscanf(r, "%f", &root_classifier[i].beta);
		root_classifier[i].root.w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, ccmalloc(ccv_compute_dense_matrix_size(rows, cols, CCV_32F | 31)), 0);
		size += ccv_compute_dense_matrix_size(rows, cols, CCV_32F | 31);
		for (j = 0; j < rows * cols * 31; j++)
			fscanf(r, "%f", &root_classifier[i].root.w->data.f32[j]);
		ccv_make_matrix_immutable(root_classifier[i].root.w);
		fscanf(r, "%d", &root_classifier[i].count);
		ccv_dpm_part_classifier_t* part_classifier = (ccv_dpm_part_classifier_t*)ccmalloc(sizeof(ccv_dpm_part_classifier_t) * root_classifier[i].count);
		size += sizeof(ccv_dpm_part_classifier_t) * root_classifier[i].count;
		for (j = 0; j < root_classifier[i].count; j++)
		{
			fscanf(r, "%d %d %d", &part_classifier[j].x, &part_classifier[j].y, &part_classifier[j].z);
			fscanf(r, "%lf %lf %lf %lf", &part_classifier[j].dx, &part_classifier[j].dy, &part_classifier[j].dxx, &part_classifier[j].dyy);
			// printf("%lf %lf %lf %lf\n", part_classifier[j].dx, part_classifier[j].dy, part_classifier[j].dxx, part_classifier[j].dyy);
			fscanf(r, "%d %d %d", &rows, &cols, &part_classifier[j].counterpart);
			part_classifier[j].w = ccv_dense_matrix_new(rows, cols, CCV_32F | 31, ccmalloc(ccv_compute_dense_matrix_size(rows, cols, CCV_32F | 31)), 0);
			size += ccv_compute_dense_matrix_size(rows, cols, CCV_32F | 31);
			for (k = 0; k < rows * cols * 31; k++)
				fscanf(r, "%f", &part_classifier[j].w->data.f32[k]);
			ccv_make_matrix_immutable(part_classifier[j].w);
		}
		root_classifier[i].part = part_classifier;
	}
	fclose(r);
	unsigned char* m = (unsigned char*)ccmalloc(size);
	ccv_dpm_mixture_model_t* model = (ccv_dpm_mixture_model_t*)m;
	m += sizeof(ccv_dpm_mixture_model_t);
	model->count = count;
	model->root = (ccv_dpm_root_classifier_t*)m;
	m += sizeof(ccv_dpm_root_classifier_t) * model->count;
	memcpy(model->root, root_classifier, sizeof(ccv_dpm_root_classifier_t) * model->count);
	for (i = 0; i < model->count; i++)
	{
		ccv_dpm_part_classifier_t* part_classifier = model->root[i].part;
		model->root[i].part = (ccv_dpm_part_classifier_t*)m;
		m += sizeof(ccv_dpm_part_classifier_t) * model->root[i].count;
		memcpy(model->root[i].part, part_classifier, sizeof(ccv_dpm_part_classifier_t) * model->root[i].count);
	}
	for (i = 0; i < model->count; i++)
	{
		ccv_dense_matrix_t* w = model->root[i].root.w;
		model->root[i].root.w = (ccv_dense_matrix_t*)m;
		m += ccv_compute_dense_matrix_size(w->rows, w->cols, w->type);
		memcpy(model->root[i].root.w, w, ccv_compute_dense_matrix_size(w->rows, w->cols, w->type));
		model->root[i].root.w->data.u8 = (unsigned char*)(model->root[i].root.w + 1);
		ccfree(w);
		for (j = 0; j < model->root[i].count; j++)
		{
			w = model->root[i].part[j].w;
			model->root[i].part[j].w = (ccv_dense_matrix_t*)m;
			m += ccv_compute_dense_matrix_size(w->rows, w->cols, w->type);
			memcpy(model->root[i].part[j].w, w, ccv_compute_dense_matrix_size(w->rows, w->cols, w->type));
			model->root[i].part[j].w->data.u8 = (unsigned char*)(model->root[i].part[j].w + 1);
			ccfree(w);
		}
	}
	return model;
}

void ccv_dpm_mixture_model_free(ccv_dpm_mixture_model_t* model)
{
	ccfree(model);
}