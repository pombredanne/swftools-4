#include <stdlib.h>
#include <math.h>
#include <string.h>
#include "../gfxdevice.h"
#include "../mem.h"
#include "poly.h"
#include "convert.h"

/* factor that determines into how many line fragments a spline is converted */
#define SUBFRACTION (2.4)

static inline int32_t convert_coord(double x, double z)
{
    /* we clamp to 31 bit instead of 32 bit because we use
       a (x1-x2) shortcut when comparing coordinates
    */
    x *= z;
    if(x < -0x40000000) x = -0x40000000;
    if(x >  0x3fffffff) x =  0x3fffffff;
    return ceil(x);
}

static void convert_gfxline(gfxline_t*line, polywriter_t*w, double gridsize)
{
    assert(!line || line[0].type == gfx_moveTo);
    double lastx=0,lasty=0;
    double z = 1.0 / gridsize;
    while(line) {
        if(line->type == gfx_moveTo) {
	    if(line->next && line->next->type != gfx_moveTo && (line->x!=lastx || line->y!=lasty)) {
		w->moveto(w, convert_coord(line->x,z), convert_coord(line->y,z));
	    }
        } else if(line->type == gfx_lineTo) {
            w->lineto(w, convert_coord(line->x,z), convert_coord(line->y,z));
	} else if(line->type == gfx_splineTo) {
            int parts = (int)(sqrt(fabs(line->x-2*line->sx+lastx) + 
                                   fabs(line->y-2*line->sy+lasty))*SUBFRACTION);
            if(!parts) parts = 1;
	    double stepsize = 1.0/parts;
            int i;
	    for(i=0;i<parts;i++) {
		double t = (double)i*stepsize;
		double sx = (line->x*t*t + 2*line->sx*t*(1-t) + lastx*(1-t)*(1-t));
		double sy = (line->y*t*t + 2*line->sy*t*(1-t) + lasty*(1-t)*(1-t));
		w->lineto(w, convert_coord(sx,z), convert_coord(sy,z));
	    }
	    w->lineto(w, convert_coord(line->x,z), convert_coord(line->y,z));
        }
	lastx = line->x;
	lasty = line->y;
        line = line->next;
    }
}

static char* readline(FILE*fi)
{
    char c;
    while(1) {
        int l = fread(&c, 1, 1, fi);
        if(!l)
            return 0;
        if(c!=10 || c!=13)
            break;
    }
    char line[256];
    int pos = 0;
    while(1) {
        line[pos++] = c;
        line[pos] = 0;
        int l = fread(&c, 1, 1, fi);
        if(!l || c==10 || c==13) {
            return strdup(line);
        }
    }
}

static void convert_file(const char*filename, polywriter_t*w, double gridsize)
{
    FILE*fi = fopen(filename, "rb");
    if(!fi) {
        perror(filename);
    }
    double z = 1.0 / gridsize;
    int count = 0;
    double g = 0;
    double lastx=0,lasty=0;
    while(1) {
        char*line = readline(fi);
        if(!line)
            break;
        double x,y;
        char s[256];
        if(sscanf(line, "%lf %lf %s", &x, &y, &s) == 3) {
            if(s && !strcmp(s,"moveto")) {
		w->moveto(w, convert_coord(x,z), convert_coord(y,z));
                count++;
            } else if(s && !strcmp(s,"lineto")) {
		w->lineto(w, convert_coord(x,z), convert_coord(y,z));
                count++;
            } else {
                fprintf(stderr, "invalid command: %s\n", s);
            }
        } else if(sscanf(line, "%% gridsize %lf", &g) == 1) {
	    gridsize = g;
	    z = 1.0 / gridsize;
	    w->setgridsize(w, g);
        }
        free(line);
    }
    fclose(fi);
    if(g) {
        fprintf(stderr, "loaded %d points from %s (gridsize %f)\n", count, filename, g);
    } else {
        fprintf(stderr, "loaded %d points from %s\n", count, filename);
    }
}

typedef struct _compactpoly {
    gfxpoly_t*poly;
    point_t last;
    point_t*points;
    int num_points;
    int points_size;
    segment_dir_t dir;
    char new;
} compactpoly_t;

void finish_segment(compactpoly_t*data)
{
    if(data->num_points <= 1)
	return;
    point_t*p = malloc(sizeof(point_t)*data->num_points);
    gfxpolystroke_t*s = rfx_calloc(sizeof(gfxpolystroke_t));
    s->next = data->poly->strokes;
    data->poly->strokes = s;
    s->num_points = s->points_size = data->num_points;
    s->dir = data->dir;
    s->points = p;
    assert(data->dir != DIR_UNKNOWN);
    if(data->dir == DIR_UP) {
	int t;
	int s = data->num_points;
	for(t=0;t<data->num_points;t++) {
	    p[--s] = data->points[t];
	}
    } else {
	memcpy(p, data->points, sizeof(point_t)*data->num_points);
    }
#ifdef CHECKS
    int t;
    for(t=0;t<data->num_points-1;t++) {
	assert(p[t].y<=p[t+1].y);
    }
#endif
}
static void compactmoveto(polywriter_t*w, int32_t x, int32_t y)
{
    compactpoly_t*data = (compactpoly_t*)w->internal;
    point_t p;
    p.x = x;
    p.y = y;
    if(p.x != data->last.x || p.y != data->last.y) {
	data->new = 1;
    }
    data->last = p;
}
static void compactlineto(polywriter_t*w, int32_t x, int32_t y)
{
    compactpoly_t*data = (compactpoly_t*)w->internal;
    point_t p;
    p.x = x;
    p.y = y;
    if(p.x == data->last.x && p.y == data->last.y)
	return;

    if(p.y < data->last.y && data->dir != DIR_UP ||
       p.y > data->last.y && data->dir != DIR_DOWN || 
       data->new) {
	finish_segment(data);
	data->dir = p.y > data->last.y ? DIR_DOWN : DIR_UP;
	data->points[0] = data->last;
	data->num_points = 1;
    }
    data->new = 0;

    if(data->points_size == data->num_points) {
	data->points_size <<= 1;
	assert(data->points_size > data->num_points);
	data->points = rfx_realloc(data->points, sizeof(point_t)*data->points_size);
    }
    data->points[data->num_points++] = p;
    data->last = p;
}
static void compactsetgridsize(polywriter_t*w, double gridsize)
{
    compactpoly_t*d = (compactpoly_t*)w->internal;
    d->poly->gridsize = gridsize;
}
/*static int compare_stroke(const void*_s1, const void*_s2)
{
    gfxpolystroke_t*s1 = (gfxpolystroke_t*)_s1;
    gfxpolystroke_t*s2 = (gfxpolystroke_t*)_s2;
    return s1->points[0].y - s2->points[0].y;
}*/
static void*compactfinish(polywriter_t*w)
{
    compactpoly_t*data = (compactpoly_t*)w->internal;
    finish_segment(data);
    //qsort(data->poly->strokes, data->poly->num_strokes, sizeof(gfxpolystroke_t), compare_stroke);
    free(data->points);
    gfxpoly_t*poly = data->poly;
    free(w->internal);w->internal = 0;
    return (void*)poly;
}
void gfxpolywriter_init(polywriter_t*w)
{
    w->moveto = compactmoveto;
    w->lineto = compactlineto;
    w->setgridsize = compactsetgridsize;
    w->finish = compactfinish;
    compactpoly_t*data = w->internal = rfx_calloc(sizeof(compactpoly_t));
    data->poly = rfx_calloc(sizeof(gfxpoly_t));
    data->poly->gridsize = 1.0;
    data->last.x = data->last.y = 0;
    data->num_points = 0;
    data->points_size = 16;
    data->new = 1;
    data->dir = DIR_UNKNOWN;
    data->points = (point_t*)rfx_alloc(sizeof(point_t)*data->points_size);
    data->poly->strokes = 0;
}

gfxpoly_t* gfxpoly_from_gfxline(gfxline_t*line, double gridsize)
{
    polywriter_t writer;
    gfxpolywriter_init(&writer);
    writer.setgridsize(&writer, gridsize);
    convert_gfxline(line, &writer, gridsize);
    return (gfxpoly_t*)writer.finish(&writer);
}
gfxpoly_t* gfxpoly_from_file(const char*filename, double gridsize)
{
    polywriter_t writer;
    gfxpolywriter_init(&writer);
    writer.setgridsize(&writer, gridsize);
    convert_file(filename, &writer, gridsize);
    return (gfxpoly_t*)writer.finish(&writer);
}
void gfxpoly_destroy(gfxpoly_t*poly)
{
    int t;
    gfxpolystroke_t*stroke = poly->strokes;
    while(stroke) {
	gfxpolystroke_t*next = stroke->next;
	free(stroke->points);
	free(stroke);
	stroke = next;
    }
    free(poly);
}

