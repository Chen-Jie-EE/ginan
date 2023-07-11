
#include "coordinates.hpp"
#include "ionoModel.hpp"
#include "observations.hpp"
#include "common.hpp"
#include "acsConfig.hpp"

#define LEG_ITER_NUM 100
#define LEG_EPSILON0 (1.e-7) // Legendre function accuracy
#define LEG_EPSILON1 (1.e-7) // Legendre function degree accuracy
#define SQR(x)  ((x)*(x))

struct Scp_Basis
{
	int hind;						/* layer number */
	int order;						/* order of the legendre function */
	double degree;					/* degree of the function */
	bool parity;					/* longitude function: false=cosine, true=sine */
};

map<int, Scp_Basis>  Scp_Basis_list;
double scap_rotmtx[9] = {};
double scap_maxlat = PI / 2;

/*-----------------------------------------------------
P=leg(m,n,x) Returns the legendre function @ x
m: order
n: degree
x: argument (rad)
Truncation is set up by the resolution parameter eps0
-----------------------------------------------------
Author: German Olivares @ GA 17 January 2019
-----------------------------------------------------*/
double legendre_function(int m, double n, double x)
{
	double A, Kmn, P, Ptmp;
	double eps = 10 * LEG_EPSILON0;
	int j = 1;

	if (m == 0) 
		A = 1;
	else
	{
		double p = pow(n / m, 2) - 1;
		double e1 = -(1 + 1 / p) / (12 * m);
		double e2 = (1 + 3 / pow(p, 2) + 4 / pow(p, 3)) / (360 * pow(m, 3));
		Kmn = pow(2, -m) * pow((n + m) / (n - m), (n + 2) / 4) * pow(p, m / 2.0) * exp(e1 + e2) / sqrt(m * PI);
		A = Kmn * pow(sin(x), m);
	}

	P = A;

	while ( eps > LEG_EPSILON0 )
	{
		Ptmp = P;
		A = ((j + m - 1) * (j + m) - n * (n + 1)) / (j * (j + m)) * A;
		P = P + A * pow(sin(x / 2), 2 * j);
		eps = abs((Ptmp - P) / Ptmp);
		j = j + 1;
	}

	return (P);
}

/*-----------------------------------------------------
dP=dleg(m,n,x) Returns the derivative of the legendre function @ x
m: order
n: degree
x: argument (rad)
Truncation is set up by the resolution parameter eps0
---------------------------------------------------------------------
Author: German Olivares @ GA 17 January 2019
---------------------------------------------------------------------*/
double legendre_derivatv(int m, double n, double x)
{
	double A, Kmn, dP, P, Ptmp, dPtmp;
	double eps = 10 * LEG_EPSILON0;
	double p, e1, e2;
	int i = 1;

	if ( m == 0 )
	{
		Kmn = 1;
		A = 1;
	}
	else
	{
		p = pow(n / m, 2) - 1;
		e1 = -(1 + 1 / p) / (12 * m);
		e2 = (1 + 3 / pow(p, 2) + 4 / pow(p, 3)) / (360 * pow(m, 3));
		Kmn = pow(2, -m) * pow((n + m) / (n - m), (n + 2) / 4) * pow(p, m / 2.0) * exp(e1 + e2) / sqrt(m * PI);
		A = Kmn * pow(sin(x), m);
	}

	P = A;

	while ( eps > LEG_EPSILON0 )
	{
		Ptmp = P;
		A = ((i + m - 1) * (i + m) - n * (n + 1)) / (i * (i + m)) * A;
		P = P + A * pow(sin(x / 2), 2 * i);
		eps = fabs((Ptmp - P) / Ptmp);
		i = i + 1;
	}

	if ( m == 0 || x == PI / 2 )
		dP = 0;
	else
		dP = m * cos(x) / sin(x) * P;

	// Compute intial A for dP
	A = Kmn * pow(sin(x), m);

	eps = 10 * LEG_EPSILON0;
	i = 1;

	while ( eps > LEG_EPSILON0 )
	{
		dPtmp = dP;
		A = ((i + m - 1) * (i + m) - n * (n + 1)) / (i * (i + m)) * A;
		dP = dP + sin(x) / 2 * A * i * pow(sin(x / 2), 2 * (i - 1));
		eps = fabs((dPtmp - dP) / dPtmp);
		i = i + 1;
	}

	return (dP);
}

/* Returns the root of legendre functions in the interval (ntmp[0], ntmp[1]) */
double bisection(int m, double* ntmp, double x, int nu)
{
	double Pa, Pd, err;
	double a = ntmp[0], b = ntmp[1];
	double d = (a + b) / 2;
	int step = 0;

	/* Compute the Legendre function value between a and b */
	if ((nu - m) % 2 == 0)
	{
		// Even boundary conditions
		Pa = legendre_derivatv(m, a, x);
		Pd = legendre_derivatv(m, d, x);
	}
	else
	{
		/* Odd boundary conditions */
		Pa = legendre_function(m, a, x);
		Pd = legendre_function(m, d, x);
	}

	for (step = 0, err = fabs(Pd); step < LEG_ITER_NUM; step++)
	{
		if (err < LEG_EPSILON0) 
			break;

		if ( Pa * Pd < 0 ) 
			b = d;
		else 
			a = d;

		d = (a + b) / 2;

		if ( (m - nu) % 2 == 0)
		{
			/* Even boundary conditions */
			Pa = legendre_derivatv(m, a, x);
			Pd = legendre_derivatv(m, d, x);
		}
		else
		{
			/* Odd boundary conditions */
			Pa = legendre_function(m, a, x);
			Pd = legendre_function(m, d, x);
		}

		err = fabs(Pd);
	}

	return (d);
}

/** transforms the Ionosphere
Piercing Point and checks if it falls in area of coverage
time:  		I 		time of observations (not used)
IPP: 			I 		Ionospheric piercing point to be updated
returns 1 if the IPP is within the area of coverage
-----------------------------------------------------
Author: Ken Harima @ RMIT 01 August 2020
-----------------------------------------------------*/
bool ippCheckSphcap(
	GTime		time, 
	VectorPos&	ionPP)
{
	VectorPos pos;
	pos[0] = ionPP[0];
	pos[1] = ionPP[1];
	pos[2] = acsConfig.ionModelOpts.layer_heights[0];
	
	VectorEcef	rpp = pos2ecef(pos);
	
	VectorEcef rrot;
	matmul("NN", 3, 1, 3, 1, scap_rotmtx, rpp.data(), 0, rrot.data());
	
	pos = ecef2pos(rrot);

	ionPP[0] = PI / 2 - pos.lat();			/* colatitude for spherical harmonic caps */

	if (ionPP[0] > scap_maxlat) 
		return false;

	ionPP[1] = pos[1];

	return true;
}


/** Evaluates spherical cap harmonics basis functions
	int ind			I		Basis function number
	obs				I		Ionosphere measurement struct
		latIPP				- Latitude of Ionosphere Piercing Point
		lonIPP				- Longitude of Ionosphere Piercing Point
		angIPP				- Angular gain for Ionosphere Piercing Point
	bool slant		I		false: output coefficient for Vtec, true: output coefficient for delay
----------------------------------------------------------------------------*/
double ionCoefSphcap(
	int			ind, 
	IonoObs&	obs,
	bool		slant)
{
	if (ind >= Scp_Basis_list.size()) 
		return 0;

	Scp_Basis& basis = Scp_Basis_list[ind];

	double legr	= legendre_function(basis.order, basis.degree, obs.ippMap[basis.hind].lat);		// Legendre function

	double out;

	if (basis.parity)	out = legr * sin(basis.order * obs.ippMap[basis.hind].lon);
	else				out = legr * cos(basis.order * obs.ippMap[basis.hind].lon);

	if (slant)
	{
		out *= obs.ippMap[basis.hind].slantFactor * obs.stecToDelay;
	}

	return out;
}

/** Estimate Ionosphere VTEC using Spherical Cap Harmonic models
	gtime_t  time		I		time of solutions (not useful for this one
	Ion_pp				I		Ionosphere Piercing Point
	layer				I 		Layer number
	vari				O		variance of VTEC
returns: VETC at piercing point
----------------------------------------------------------------------------*/
double ionVtecSphcap(
    GTime		time,
    VectorPos&	ionPP,
    int			layer,
    double&		var,
    KFState&	kfState)
{
	if (ippCheckSphcap(time, ionPP) == false)
	{
		var = 0;
		return 0;
	}

	var = 0;
	double iono = 0;
	GObs tmpobs;
	tmpobs.ippMap[layer].lat			= ionPP[0];
	tmpobs.ippMap[layer].lon			= ionPP[1];
	tmpobs.ippMap[layer].slantFactor	= 1;

	for (int ind = 0; ind < acsConfig.ionModelOpts.numBasis; ind++)
	{
		Scp_Basis& basis = Scp_Basis_list[ind];

		if (basis.hind != layer)
			continue;

		double coef = ionCoefSphcap(ind, tmpobs, false);

		KFKey keyC;
		keyC.type	= KF::IONOSPHERIC;
		keyC.num	= ind;

		double staval = 0;
		double stastd = 0;
		kfState.getKFValue(keyC, staval, &stastd);

		iono	+= 		coef * staval;
		var		+= SQR( coef * stastd);
	}

	return iono;
}


/** Initializes Spherical caps Ionosphere model
	The following configursation parameters are used
	-  acsConfig.ionoOpts.lat_center:    latitude of map centre
	-  acsConfig.ionoOpts.lon_center:    longitude of map centre
	-  acsConfig.ionoOpts.lat_width:	  latitude width of maps
	-  acsConfig.ionoOpts.lon_width:	  longitude width of maps
	-  acsConfig.ionoOpts.func_order:	  Legendre function order
	-  acsConfig.ionoOpts.layer_heights: Ionosphere layer Heights
----------------------------------------------------------------------------*/
int configIonModelSphcap()
{
	double latc = acsConfig.ionexGrid.lat_center	* D2R;
	double lonc = acsConfig.ionexGrid.lon_center	* D2R;
	double latw = acsConfig.ionexGrid.lat_width		* D2R / 2;
	double lonw = acsConfig.ionexGrid.lon_width		* D2R / 2;
	scap_rotmtx[0] =  sin(latc) * cos(lonc);
	scap_rotmtx[1] =  sin(latc) * sin(lonc);
	scap_rotmtx[2] = -cos(latc);
	scap_rotmtx[3] = -sin(lonc);
	scap_rotmtx[4] =  cos(lonc);
	scap_rotmtx[5] =  0;
	scap_rotmtx[6] =  cos(latc) * cos(lonc);
	scap_rotmtx[7] =  cos(latc) * sin(lonc);
	scap_rotmtx[8] =  sin(latc);
	scap_maxlat = acos(cos(latc) * cos(latc + latw) * (cos(lonw) - 1) + cos(latw));

	if	(  scap_maxlat > 0.45 * PI 
		&& scap_maxlat > 0.5 * PI) 
	{
		scap_maxlat = PI / 2;
	}
	
	int Kmax = acsConfig.ionModelOpts.function_order;
	int nlay = acsConfig.ionModelOpts.layer_heights.size();
	int ind = 0;
	Scp_Basis basis;

	for (int lay = 0; lay < nlay; lay++)
	{
		basis.hind = lay;

		for (int m = 0; m <= Kmax; m++)
		{
			basis.order = m;
			double nodd[2] = {m + 0.05, m + 0.10};
			double neve[2] = {m + 0.05, m + 0.10};

			for (int k = m; k <= Kmax; k++)
			{
				double nk = 0;

				if (scap_maxlat == PI / 2) 
					nk = k;
				else if (k == 0) 
					nk = 0;
				else if ((k - m) % 2)
				{
					while (1)
					{
						nodd[1] = nodd[0] + 0.5;
						double p1 = legendre_function(m, nodd[0], scap_maxlat);
						double p2 = legendre_function(m, nodd[1], scap_maxlat);

						if (fabs(p1) < LEG_EPSILON0)
						{
							nk = nodd[0];
							nodd[0] += 0.5;
						}

						if (fabs(p2) < LEG_EPSILON0)
						{
							nk = nodd[1];
							nodd[0] = nodd[1];
							break;
						}

						if ((p1 * p2) < 0)
						{
							nk = bisection(m, nodd, scap_maxlat, k);

							if (fabs(nk - m) < LEG_EPSILON1) nk = 1.0 * m;

							nodd[0] = nk + 0.1;
						}
						else 
							nodd[0] = nodd[1];
					}
				}
				else
				{
					while (1)
					{
						neve[1] = neve[0] + 0.5;
						double p1 = legendre_derivatv(m, neve[0], scap_maxlat);
						double p2 = legendre_derivatv(m, neve[1], scap_maxlat);

						if (fabs(p1) < LEG_EPSILON0)
						{
							nk = neve[0];
							neve[0] += 0.5;
						}

						if (fabs(p2) < LEG_EPSILON0)
						{
							nk = neve[1];
							neve[0] = neve[1];
							break;
						}

						if ((p1 * p2) < 0)
						{
							nk = bisection(m, neve, scap_maxlat, k);

							if (fabs(nk - m) < LEG_EPSILON1) nk = 1.0 * m;

							neve[0] = nk + 0.1;
						}
						else neve[0] = neve[1];
					}
				}

				basis.degree = nk;
				basis.parity = false;
				Scp_Basis_list[ind++] = basis;

				if (m > 0)
				{
					basis.parity = true;
					Scp_Basis_list[ind++] = basis;
				}
			}
		}
	}

	acsConfig.ionModelOpts.numBasis = ind;

	for (int j = 0; j < acsConfig.ionModelOpts.numBasis; j++)
	{
		Scp_Basis& basis = Scp_Basis_list[j];
// 		fprintf(fp_iondebug, "SCP_BASIS %3d %2d %2d %8.4f %1d ", j, basis.hind, basis.order, basis.degree, basis.parity);
// 		fprintf(fp_iondebug, "\n");
	}

	return ind;
}
