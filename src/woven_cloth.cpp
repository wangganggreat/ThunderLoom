#include "woven_cloth.h"
#include "wif/wif.cpp"
#include "wif/ini.cpp"

// For M_PI etc.
#define _USE_MATH_DEFINES
#include <math.h>

// -- 3D Vector data structure -- //
typedef struct
{
    float x,y,z,w;
} wcVector;

static wcVector wcvector(float x, float y, float z)
{
    wcVector ret = {x,y,z,0.f};
    return ret;
}

static float wcVector_magnitude(wcVector v)
{
    return sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
}

static wcVector wcVector_normalize(wcVector v)
{
    wcVector ret;
    float inv_mag = 1.f/sqrtf(v.x*v.x + v.y*v.y + v.z*v.z);
    ret.x = v.x * inv_mag;
    ret.y = v.y * inv_mag;
    ret.z = v.z * inv_mag;
    return ret;
}

static float wcVector_dot(wcVector a, wcVector b)
{
    return a.x*b.x + a.y*b.y + a.z*b.z;
}

static wcVector wcVector_cross(wcVector a, wcVector b)
{
    wcVector ret;
    ret.x = a.y*b.z - a.z*b.y;
    ret.y = a.z*b.x - a.x*b.z;
    ret.z = a.x*b.y - a.y*b.x;
    return ret;
}

static wcVector wcVector_add(wcVector a, wcVector b)
{
    wcVector ret;
    ret.x = a.x + b.x;
    ret.y = a.y + b.y;
    ret.z = a.z + b.z;
    return ret;
}

// -- //

//Returns a random float in the range [0,1]
//TODO(Vidar): Better rng
float get_random_float()
{
    return (float)rand()/(float)(RAND_MAX);
}

//From Mitsuba
static uint64_t sampleTEA(uint32_t v0, uint32_t v1, int rounds = 4)
{
	uint32_t sum = 0;

	for (int i=0; i<rounds; ++i) {
		sum += 0x9e3779b9;
		v0 += ((v1 << 4) + 0xA341316C) ^ (v1 + sum) ^ ((v1 >> 5) + 0xC8013EA4);
		v1 += ((v0 << 4) + 0xAD90777D) ^ (v0 + sum) ^ ((v0 >> 5) + 0x7E95761E);
	}

	return ((uint64_t) v1 << 32) + v0;
}

//From Mitsuba
static float sampleTEASingle(uint32_t v0, uint32_t v1, int rounds = 4)
{
    /* Trick from MTGP: generate an uniformly distributed
    single precision number in [1,2) and subtract 1. */
    union {
        uint32_t u;
        float f;
    } x;
    x.u = ((sampleTEA(v0, v1, rounds) & 0xFFFFFFFF) >> 9) | 0x3f800000UL;
    return x.f - 1.0f;
}

void sample_cosine_hemisphere(float *p_x, float *p_y, float *p_z)
{
    //sample uniform disk concentric
    // From mitsuba warp.cpp
    float sample_x = get_random_float();
    float sample_y = get_random_float();
	float r1 = 2.0f*sample_x - 1.0f;
	float r2 = 2.0f*sample_y - 1.0f;

	/* Modified concencric map code with less branching (by Dave Cline), see
	   http://psgraphics.blogspot.ch/2011/01/improved-code-for-concentric-map.html */
	float phi, r;
	if (r1 == 0 && r2 == 0) {
		r = phi = 0;
	} else if (r1*r1 > r2*r2) {
		r = r1;
		phi = (M_PI/4.0f) * (r2/r1);
	} else {
		r = r2;
		phi = (M_PI_2) - (r1/r2) * (M_PI/4.0f);
	}

    *p_x = r * cosf(phi);
    *p_x = r * sinf(phi);
    *p_z = sqrtf(1.0f - (*p_x)*(*p_x) - (*p_y)*(*p_y));
}

static void finalize_weave_parmeters(wcWeaveParameters *params)
{
    //Calculate normalization factor for the specular reflection
    //Irawan:
    /* Estimate the average reflectance under diffuse
       illumination and use it to normalize the specular
       component */

    //TODO(Vidar): Better rng... Or quasi-monte carlo?
    srand(3452); // We always want the same seed
    size_t nSamples = 10000;
    float result = 0.0f;
    params->specular_normalization = 1.f;
    for (size_t i=0; i<nSamples; ++i) {

        wcIntersectionData intersection_data;
        intersection_data.uv_x = get_random_float();
        intersection_data.uv_y = get_random_float();

        sample_cosine_hemisphere(&intersection_data.wi_x,
            &intersection_data.wi_y, &intersection_data.wi_z);

        sample_cosine_hemisphere(&intersection_data.wo_x,
            &intersection_data.wo_y, &intersection_data.wo_z);

        wcPatternData pattern_data = wcGetPatternData(
            intersection_data, params);
        result += wcEvalSpecular(intersection_data,
                pattern_data,params);
    }

    if (result <= 0.0001f){
        params->specular_normalization = 0.f;
    }else{
        params->specular_normalization = nSamples / (result * M_PI);
    }
}

void wcWeavePatternFromWIF(wcWeaveParameters *params, const char *filename)
{
    WeaveData *data = wif_read(filename);
    params->pattern_entry = wif_get_pattern(data,
        &params->pattern_width, &params->pattern_height);
    wif_free_weavedata(data);
    finalize_weave_parmeters(params);
}

void wcWeavePatternFromData(wcWeaveParameters *params, uint8_t *pattern,
    float *warp_color, float *weft_color, uint32_t pattern_width,
    uint32_t pattern_height)
{
    params->pattern_width  = pattern_width;
    params->pattern_height = pattern_height;
    params->pattern_entry  = wif_build_pattern_from_data(pattern,
            warp_color, weft_color, pattern_width, pattern_height);
    finalize_weave_parmeters(params);
}

static float intensityVariation(wcPatternData pattern_data,
    const wcWeaveParameters *params)
{
    if(params->intensity_fineness < 0.001f){
        return 1.f;
    }
    // have index to make a grid of finess*fineness squares 
    // of which to have the same brightness variations.
    
    //TODO(Peter): Clean this up a bit...
    //segment start x,y
    float startx = pattern_data.total_x - pattern_data.x*pattern_data.width;
    float starty = pattern_data.total_y - pattern_data.y*pattern_data.length;
    float centerx = startx + pattern_data.width/2.0;
    float centery = starty + pattern_data.length/2.0;
    
    uint32_t r1 = (uint32_t) ((centerx + pattern_data.total_x) 
            * params->intensity_fineness);
    uint32_t r2 = (uint32_t) ((centery + pattern_data.total_y) 
            * params->intensity_fineness);

    //srand(r1+r2); //bad way to do it?
    //float xi = rand();
    //return fmin(-math::fastlog(xi), (float) 10.0f);
    
    float xi = sampleTEASingle(r1, r2, 8);
    float log_xi = -logf(xi);
    return log_xi < 10.f ? log_xi : 10.f;
}

static void calculateLengthOfSegment(bool warp_above, uint32_t pattern_x,
                uint32_t pattern_y, uint32_t *steps_left,
                uint32_t *steps_right,  uint32_t pattern_width,
                uint32_t pattern_height, PaletteEntry *pattern_entry)
{

    uint32_t current_x = pattern_x;
    uint32_t current_y = pattern_y;
    uint32_t *incremented_coord = warp_above ? &current_y : &current_x;
    uint32_t max_size = warp_above ? pattern_height: pattern_width;
    uint32_t initial_coord = warp_above ? pattern_y: pattern_x;
    *steps_right = 0;
    *steps_left  = 0;
    do{
        (*incremented_coord)++;
        if(*incremented_coord == max_size){
            *incremented_coord = 0;
        }
        if((bool)(pattern_entry[current_x +
                current_y*pattern_width].warp_above) != warp_above){
            break;
        }
        (*steps_right)++;
    } while(*incremented_coord != initial_coord);

    *incremented_coord = initial_coord;
    do{
        if(*incremented_coord == 0){
            *incremented_coord = max_size;
        }
        (*incremented_coord)--;
        if((bool)(pattern_entry[current_x +
                current_y*pattern_width].warp_above) != warp_above){
            break;
        }
        (*steps_left)++;
    } while(*incremented_coord != initial_coord);
}

static float vonMises(float cos_x, float b) {
    // assumes a = 0, b > 0 is a concentration parameter.
    float I0, absB = fabsf(b);
    if (fabsf(b) <= 3.75f) {
        float t = absB / 3.75f;
        t = t * t;
        I0 = 1.0f + t*(3.5156229f + t*(3.0899424f + t*(1.2067492f
            + t*(0.2659732f + t*(0.0360768f + t*0.0045813f)))));
    } else {
        float t = 3.75f / absB;
        I0 = expf(absB) / sqrtf(absB) * (0.39894228f + t*(0.01328592f
            + t*(0.00225319f + t*(-0.00157565f + t*(0.00916281f
            + t*(-0.02057706f + t*(0.02635537f + t*(-0.01647633f
            + t*0.00392377f))))))));
    }

    return expf(b * cos_x) / (2 * M_PI * I0);
}

wcPatternData wcGetPatternData(wcIntersectionData intersection_data,
        const wcWeaveParameters *params) {
    float uv_x = intersection_data.uv_x;
    float uv_y = intersection_data.uv_y;

    //Set repeating uv coordinates.
    float u_repeat = fmod(uv_x*params->uscale,1.f);
    float v_repeat = fmod(uv_y*params->vscale,1.f);

    //pattern index
    //TODO(Peter): these are new. perhaps they can be used later 
    // to avoid duplicate calculations.
    //TODO(Peter): come up with a better name for these...
    uint32_t total_x = uv_x*params->uscale*params->pattern_width;
    uint32_t total_y = uv_y*params->vscale*params->pattern_height;

    //TODO(Vidar): Check why this crashes sometimes
    if (u_repeat < 0.f) {
        u_repeat = u_repeat - floor(u_repeat);
    }
    if (v_repeat < 0.f) {
        v_repeat = v_repeat - floor(v_repeat);
    }

    uint32_t pattern_x = (uint32_t)(u_repeat*(float)(params->pattern_width));
    uint32_t pattern_y = (uint32_t)(v_repeat*(float)(params->pattern_height));

    PaletteEntry current_point = params->pattern_entry[pattern_x +
        pattern_y*params->pattern_width];        

    //Calculate the size of the segment
    uint32_t steps_left_warp = 0, steps_right_warp = 0;
    uint32_t steps_left_weft = 0, steps_right_weft = 0;
    if (current_point.warp_above) {
        calculateLengthOfSegment(current_point.warp_above, pattern_x,
            pattern_y, &steps_left_warp, &steps_right_warp,
            params->pattern_height, params->pattern_width,
            params->pattern_entry);
    }else{
        calculateLengthOfSegment(current_point.warp_above, pattern_x,
            pattern_y, &steps_left_weft, &steps_right_weft,
            params->pattern_height, params->pattern_width,
            params->pattern_entry);
    }

    //Yarn-segment-local coordinates.
    float l = (steps_left_warp + steps_right_warp + 1.f);
    float y = ((v_repeat*(float)(params->pattern_height) - (float)pattern_y)
            + steps_left_warp)/l;

    float w = (steps_left_weft + steps_right_weft + 1.f);
    float x = ((u_repeat*(float)(params->pattern_width) - (float)pattern_x)
            + steps_left_weft)/w;

    //Rescale x and y to [-1,1]
    x = x*2.f - 1.f;
    y = y*2.f - 1.f;

    //Switch X and Y for warp, so that we always have the yarn
    // cylinder going along the y axis
    if(!current_point.warp_above){
        float tmp1 = x;
        float tmp2 = w;
        x = -y;
        y = tmp1;
        w = l;
        l = tmp2;
    }

    //Calculate the yarn-segment-local u v coordinates along the curved cylinder
    //NOTE: This is different from how Irawan does it
    /*segment_u = asinf(x*sinf(params->umax));
        segment_v = asinf(y);*/
    //TODO(Vidar): Use a parameter for choosing model?
    float segment_u = y*params->umax;
    float segment_v = x*M_PI_2;

    //Calculate the normal in thread-local coordinates
    float normal_x = sinf(segment_v);
    float normal_y = sinf(segment_u)*cosf(segment_v);
    float normal_z = cosf(segment_u)*cosf(segment_v);
    
    //Transform the normal back to shading space
    if(!current_point.warp_above){
        float tmp = normal_x;
        normal_x = normal_y;
        normal_y = -tmp;
    }
  
    wcPatternData ret_data = {};
    ret_data.color_r = current_point.color[0];
    ret_data.color_g = current_point.color[1];
    ret_data.color_b = current_point.color[2];

    ret_data.u = segment_u;
    ret_data.v = segment_v;
    ret_data.length = l; 
    ret_data.width  = w; 
    ret_data.x = x; 
    ret_data.y = y; 
    ret_data.warp_above = current_point.warp_above; 
    ret_data.normal_x = normal_x;
    ret_data.normal_y = normal_y;
    ret_data.normal_z = normal_z;
    ret_data.total_x = total_x; 
    ret_data.total_y = total_y; 

    //return the results
    return ret_data;
}

float wcEvalFilamentSpecular(wcIntersectionData intersection_data,
    wcPatternData data, const wcWeaveParameters *params)
{

    wcVector wi = wcvector(intersection_data.wi_x, intersection_data.wi_y,
        intersection_data.wi_z);
    wcVector wo = wcvector(intersection_data.wo_x, intersection_data.wo_y,
        intersection_data.wo_z);

    if(!data.warp_above){
        float tmp2 = wi.x;
        float tmp3 = wo.x;
        wi.x = -wi.y; wi.y = tmp2;
        wo.x = -wo.y; wo.y = tmp3;
    }
    wcVector H = wcVector_normalize(wcVector_add(wi,wo));

    float v = data.v;
    float y = data.y;

    //TODO(Peter): explain from where these expressions come.
    //compute v from x using (11). Already done. We have it from data.
    //compute u(wi,v,wr) -- u as function of v. using (4)...
    float specular_u = atan2f(-H.z, H.y) + M_PI_2; //plus or minus in last t.
    //TODO(Peter): check that it indeed is just v that should be used 
    //to calculate Gu (6) in Irawans paper.
    //calculate yarn tangent.

    float reflection = 0.f;
    if (fabsf(specular_u) < params->umax) {
        // Make normal for highlights, uses v and specular_u
        wcVector highlight_normal = wcVector_normalize(wcvector(sinf(v),
                    sinf(specular_u)*cosf(v),
                    cosf(specular_u)*cosf(v)));

        // Make tangent for highlights, uses v and specular_u
        wcVector highlight_tangent = wcVector_normalize(wcvector(0.f, 
                    cosf(specular_u), -sinf(specular_u)));

        //get specular_y, using irawans transformation.
        float specular_y = specular_u/params->umax;
        // our transformation TODO(Peter): Verify!
        //float specular_y = sinf(specular_u)/sinf(m_umax);

        //Clamp specular_y TODO(Peter): change name of m_delta_x to m_delta_h
        specular_y = specular_y < 1.f - params->delta_x ? specular_y :
            1.f - params->delta_x;
        specular_y = specular_y > -1.f + params->delta_x ? specular_y :
            -1.f + params->delta_x;

        if (fabsf(specular_y - y) < params->delta_x) { //this takes the role of xi in the irawan paper.
            // --- Set Gu, using (6)
            float a = 1.f; //radius of yarn
            float R = 1.f/(sin(params->umax)); //radius of curvature
            float Gu = a*(R + a*cosf(v)) /(
                wcVector_magnitude(wcVector_add(wi,wo)) *
                fabsf((wcVector_cross(highlight_tangent,H)).x));

            // --- Set fc
            float cos_x = -wcVector_dot(wi, wo);
            float fc = params->alpha + vonMises(cos_x, params->beta);

            // --- Set A
            float widotn = wcVector_dot(wi, highlight_normal);
            float wodotn = wcVector_dot(wo, highlight_normal);
            //float A = m_sigma_s/m_sigma_t * (widotn*wodotn)/(widotn + wodotn);
            widotn = (widotn < 0.f) ? 0.f : widotn;   
            wodotn = (wodotn < 0.f) ? 0.f : wodotn;   
            float A = 0.f;
            if(widotn > 0.f && wodotn > 0.f){
                A = 1.f / (4.0 * M_PI) * (widotn*wodotn)/(widotn + wodotn); //sigmas are "unimportant"
                //TODO(Peter): Explain from where the 1/4*PI factor comes from
            }
            float l = 2.f;
            //TODO(Peter): Implement As, -- smoothes the dissapeares of the
            // higlight near the ends. Described in (9)
            reflection = 2.f*l*params->umax*fc*Gu*A/params->delta_x;
        }
    }
    return reflection;
}


float wcEvalStapleSpecular(wcIntersectionData intersection_data,
    wcPatternData data, const wcWeaveParameters *params)
{
    wcVector wi = wcvector(intersection_data.wi_x, intersection_data.wi_y,
        intersection_data.wi_z);
    wcVector wo = wcvector(intersection_data.wo_x, intersection_data.wo_y,
        intersection_data.wo_z);

    if(!data.warp_above){
        float tmp2 = wi.x;
        float tmp3 = wo.x;
        wi.x = -wi.y; wi.y = tmp2;
        wo.x = -wo.y; wo.y = tmp3;
    }
    wcVector H = wcVector_normalize(wcVector_add(wi, wo));

    float u = data.u;
    float x = data.x;
    float D;
    {
        float a = H.y*sinf(u) + H.z*cosf(u);
        D = (H.y*cosf(u)-H.z*sinf(u))/(sqrtf(H.x*H.x + a*a)) / tanf(params->psi);
    }
    float reflection = 0.f;
            
    float specular_v = atan2f(-H.y*sinf(u) - H.z*cosf(u), H.x) + acosf(D); //Plus eller minus i sista termen.
    //TODO(Vidar): Clamp specular_v, do we need it?
    // Make normal for highlights, uses u and specular_v
    wcVector highlight_normal = wcVector_normalize(wcvector(sinf(specular_v), sinf(u)*cosf(specular_v),
        cosf(u)*cosf(specular_v)));

    if (fabsf(specular_v) < M_PI_2 && fabsf(D) < 1.f) {
        //we have specular reflection
        //get specular_x, using irawans transformation.
        float specular_x = specular_v/M_PI_2;
        // our transformation
        //float specular_x = sinf(specular_v);

        //Clamp specular_x
        specular_x = specular_x < 1.f - params->delta_x ? specular_x :
            1.f - params->delta_x;
        specular_x = specular_x > -1.f + params->delta_x ? specular_x :
            -1.f + params->delta_x;

        if (fabsf(specular_x - x) < params->delta_x) { //this takes the role of xi in the irawan paper.
            // --- Set Gv
            float a = 1.f; //radius of yarn
            float R = 1.f/(sin(params->umax)); //radius of curvature
            float Gv = a*(R + a*cosf(specular_v))/(
                wcVector_magnitude(wcVector_add(wi,wo)) *
                wcVector_dot(highlight_normal,H) * fabsf(sinf(params->psi)));
            // --- Set fc
            float cos_x = -wcVector_dot(wi, wo);
            float fc = params->alpha + vonMises(cos_x, params->beta);
            // --- Set A
            float widotn = wcVector_dot(wi, highlight_normal);
            float wodotn = wcVector_dot(wo, highlight_normal);
            //float A = m_sigma_s/m_sigma_t * (widotn*wodotn)/(widotn + wodotn);
            widotn = (widotn < 0.f) ? 0.f : widotn;   
            wodotn = (wodotn < 0.f) ? 0.f : wodotn;   
            //TODO(Vidar): This is where we get the NAN
            float A = 0.f; //sigmas are "unimportant"
            if(widotn > 0.f && wodotn > 0.f){
                A = 1.f / (4.0 * M_PI) * (widotn*wodotn)/(widotn + wodotn); //sigmas are "unimportant"
                //TODO(Peter): Explain from where the 1/4*PI factor comes from
            }
            float w = 2.f;
            reflection = 2.f*w*params->umax*fc*Gv*A/params->delta_x;
        }
    }
    return reflection;
}

float wcEvalSpecular(wcIntersectionData intersection_data,
        wcPatternData data, const wcWeaveParameters *params){
    // Depending on the given psi parameter the yarn is considered
    // staple or filament. They are treated differently in order
    // to work better numerically. 
    float reflection = 0.f;
    if (params->psi <= 0.001f) {
        //Filament yarn
        reflection = wcEvalFilamentSpecular(intersection_data, data, params); 
    } else {
        //Staple yarn
        reflection = wcEvalStapleSpecular(intersection_data, data, params); 
    }
    return reflection * params->specular_normalization
        * intensityVariation(data, params) ;
}