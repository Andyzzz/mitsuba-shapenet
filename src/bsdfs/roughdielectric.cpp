/*
    This file is part of Mitsuba, a physically based rendering system.

    Copyright (c) 2007-2011 by Wenzel Jakob and others.

    Mitsuba is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License Version 3
    as published by the Free Software Foundation.

    Mitsuba is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include <mitsuba/render/bsdf.h>
#include <mitsuba/render/sampler.h>
#include <mitsuba/hw/basicshader.h>
#include "microfacet.h"
#include "ior.h"

MTS_NAMESPACE_BEGIN

/* Suggestion by Bruce Walter: sample the model using a slightly 
   wider density function. This in practice limits the importance 
   weights to values <= 4. 
*/
#define ENLARGE_LOBE_TRICK 1

/*!\plugin{roughdielectric}{Rough dielectric material}
 * \order{4}
 * \parameters{
 *     \parameter{distribution}{\String}{
 *          Specifies the type of microfacet normal distribution 
 *          used to model the surface roughness.
 *       \begin{enumerate}[(i)]
 *           \item \code{beckmann}: Physically-based distribution derived from
 *               Gaussian random surfaces. This is the default.
 *           \item \code{ggx}: New distribution proposed by
 *              Walter et al. \cite{Walter07Microfacet}, which is meant to better handle 
 *              the long tails observed in measurements of ground surfaces. 
 *              Renderings with this distribution may converge slowly.
 *           \item \code{phong}: Classical $\cos^p\theta$ distribution.
 *              Due to the underlying microfacet theory, 
 *              the use of this distribution here leads to more realistic 
 *              behavior than the separately available \pluginref{phong} plugin.
 *           \item \code{as}: Anisotropic Phong-style microfacet distribution proposed by
 *              Ashikhmin and Shirley \cite{Ashikhmin2005Anisotropic}.\vspace{-3mm}
 *       \end{enumerate}
 *     }
 *     \parameter{alpha}{\Float\Or\Texture}{
 *         Specifies the roughness of the unresolved surface micro-geometry. 
 *         When the Beckmann distribution is used, this parameter is equal to the 
 *         \emph{root mean square} (RMS) slope of the microfacets. This
 *         parameter is only valid when \texttt{distribution=beckmann/phong/ggx}.
 *         \default{0.1}. 
 *     }
 *     \parameter{alphaU, alphaV}{\Float\Or\Texture}{
 *         Specifies the anisotropic roughness values along the tangent and 
 *         bitangent directions. These parameter are only valid when 
 *         \texttt{distribution=as}. \default{0.1}. 
 *     }
 *     \parameter{intIOR}{\Float\Or\String}{Interior index of refraction specified
 *      numerically or using a known material name. \default{\texttt{bk7} / 1.5046}}
 *     \parameter{extIOR}{\Float\Or\String}{Exterior index of refraction specified
 *      numerically or using a known material name. \default{\texttt{air} / 1.000277}}
 *     \parameter{specular\showbreak Reflectance}{\Spectrum\Or\Texture}{Optional
 *         factor used to modulate the reflectance component\default{1.0}}
 *     \parameter{specular\showbreak Transmittance}{\Spectrum\Or\Texture}{Optional
 *         factor used to modulate the transmittance component\default{1.0}}
 * }\vspace{4mm}
 *
 * This plugin implements a realistic microfacet scattering model for rendering
 * rough interfaces between dielectric materials, such as a transition from air to 
 * ground glass. Microfacet theory describes rough surfaces as an arrangement of 
 * unresolved and ideally specular facets, whose normal directions are given by 
 * a specially chosen \emph{microfacet distribution}. By accounting for shadowing
 * and masking effects between these facets, it is possible to reproduce the important 
 * off-specular reflections peaks observed in real-world measurements of such 
 * materials.
 * \renderings{
 *     \rendering{Anti-glare glass (Beckmann, $\alpha=0.02$)}
 *     	   {bsdf_roughdielectric_beckmann_0_0_2.jpg}
 *     \rendering{Rough glass (Beckmann, $\alpha=0.1$)}
 *     	   {bsdf_roughdielectric_beckmann_0_1.jpg}
 * }
 *
 * This plugin is essentially the ``roughened'' equivalent of the (smooth) plugin
 * \pluginref{dielectric}. For very low values of $\alpha$, the two will
 * be very similar, though scenes using this plugin will take longer to render 
 * due to the additional computational burden of tracking surface roughness.
 * 
 * The implementation is based on the paper ``Microfacet Models
 * for Refraction through Rough Surfaces'' by Walter et al. 
 * \cite{Walter07Microfacet}. It supports several different types of microfacet
 * distributions and has a texturable roughness parameter. Exterior and 
 * interior IOR values can be specified independently, where ``exterior'' 
 * refers to the side that contains the surface normal. Similar to the 
 * \pluginref{dielectric} plugin, IOR values can either be specified 
 * numerically, or based on a list of known materials (see 
 * \tblref{dielectric-iors} for an overview). When no parameters are given, 
 * the plugin activates the default settings, which describe a borosilicate 
 * glass BK7/air interface with a light amount of roughness modeled using a 
 * Beckmann distribution.
 *
 * To get an intuition about the effect of the surface roughness
 * parameter $\alpha$, consider the following approximate differentiation: 
 * a value of $\alpha=0.001-0.01$ corresponds to a material 
 * with slight imperfections on an
 * otherwise smooth surface finish, $\alpha=0.1$ is relatively rough,
 * and $\alpha=0.3-0.7$ is \emph{extremely} rough (e.g. an etched or ground
 * finish).
 * 
 * Please note that when using this plugin, it is crucial that the scene contains
 * meaningful and mutually compatible index of refraction changes---see
 * \figref{glass-explanation} for an example of what this entails. Also, note that
 * the importance sampling implementation of this model is close, but 
 * not always a perfect a perfect match to the underlying scattering distribution,
 * particularly for high roughness values and when the \texttt{ggx} 
 * microfacet distribution is used. Hence, such renderings may 
 * converge slowly.
 *
 * \subsubsection*{Technical details}
 * When rendering with the Ashikhmin-Shirley or Phong microfacet 
 * distributions, a conversion is used to turn the specified 
 * $\alpha$ roughness value into the exponents of these distributions.
 * This is done in a way, such that the different 
 * distributions all produce a similar appearance for the same value of 
 * $\alpha$.
 *
 * The Ashikhmin-Shirley microfacet distribution allows the specification
 * of two distinct roughness values along the tangent and bitangent
 * directions. This can be used to provide a material with a ``brushed''
 * appearance. The alignment of the anisotropy will follow the UV
 * parameterization of the underlying mesh in this case. This also means that
 * such an anisotropic material cannot be applied to triangle meshes that 
 * are missing texture coordinates.\newpage
 *
 * \renderings{
 *     \rendering{Ground glass (GGX, $\alpha$=0.304, 
 *     	   \lstref{roughdielectric-roughglass})}{bsdf_roughdielectric_ggx_0_304.jpg}
 *     \rendering{Textured roughness (\lstref{roughdielectric-textured})}
 *         {bsdf_roughdielectric_textured.jpg}
 * }
 *
 * \begin{xml}[caption=A material definition for ground glass, label=lst:roughdielectric-roughglass]
 * <bsdf type="roughdielectric">
 *     <string name="distribution" value="ggx"/>
 *     <float name="alpha" value="0.304"/>
 *     <string name="intIOR" value="bk7"/>
 *     <string name="extIOR" value="air"/>
 * </bsdf>
 * \end{xml}
 *
 * \begin{xml}[caption=A texture can be attached to the roughness parameter, label=lst:roughdielectric-textured]
 * <bsdf type="roughdielectric">
 *     <string name="distribution" value="beckmann"/>
 *     <float name="intIOR" value="1.5046"/>
 *     <float name="extIOR" value="1.0"/>
 *
 *     <texture name="alpha" type="bitmap">
 *         <string name="filename" value="roughness.exr"/>
 *     </texture>
 * </bsdf>
 * \end{xml}
 */
class RoughDielectric : public BSDF {
public:
	RoughDielectric(const Properties &props) : BSDF(props) {
		m_specularReflectance = new ConstantSpectrumTexture(
			props.getSpectrum("specularReflectance", Spectrum(1.0f)));
		m_specularTransmittance = new ConstantSpectrumTexture(
			props.getSpectrum("specularTransmittance", Spectrum(1.0f)));

		/* Specifies the internal index of refraction at the interface */
		m_intIOR = lookupIOR(props, "intIOR", "bk7");

		/* Specifies the external index of refraction at the interface */
		m_extIOR = lookupIOR(props, "extIOR", "air");

		if (m_intIOR < 0 || m_extIOR < 0 || m_intIOR == m_extIOR)
			Log(EError, "The interior and exterior indices of "
				"refraction must be positive and differ!");

		m_distribution = MicrofacetDistribution(
			props.getString("distribution", "beckmann")
		);

		Float alpha = props.getFloat("alpha", 0.1f),
			  alphaU = props.getFloat("alphaU", alpha),
			  alphaV = props.getFloat("alphaV", alpha);

		m_alphaU = new ConstantFloatTexture(alphaU);
		if (alphaU == alphaV)
			m_alphaV = m_alphaU;
		else
			m_alphaV = new ConstantFloatTexture(alphaV);
	}

	RoughDielectric(Stream *stream, InstanceManager *manager) 
	 : BSDF(stream, manager) {
		m_distribution = MicrofacetDistribution(
			(MicrofacetDistribution::EType) stream->readUInt()
		);
		m_alphaU = static_cast<Texture *>(manager->getInstance(stream));
		m_alphaV = static_cast<Texture *>(manager->getInstance(stream));
		m_specularReflectance = static_cast<Texture *>(manager->getInstance(stream));
		m_specularTransmittance = static_cast<Texture *>(manager->getInstance(stream));
		m_intIOR = stream->readFloat();
		m_extIOR = stream->readFloat();

		configure();
	}

	void configure() {
		unsigned int extraFlags = 0;
		if (m_alphaU != m_alphaV) {
			extraFlags |= EAnisotropic;
			if (m_distribution.getType() != 
				MicrofacetDistribution::EAshikhminShirley)
				Log(EError, "Different roughness values along the tangent and "
						"bitangent directions are only supported when using the "
						"anisotropic Ashikhmin-Shirley microfacet distribution "
						"(named \"as\")");
		}

		if (!m_alphaU->isConstant() || !m_alphaV->isConstant())
			extraFlags |= ESpatiallyVarying;

		m_components.clear();
		m_components.push_back(EGlossyReflection | EFrontSide
			| EBackSide | ECanUseSampler | extraFlags 
			| (m_specularReflectance->isConstant() ? 0 : ESpatiallyVarying));
		m_components.push_back(EGlossyTransmission | EFrontSide
			| EBackSide | ECanUseSampler | extraFlags
			| (m_specularTransmittance->isConstant() ? 0 : ESpatiallyVarying));

		/* Verify the input parameters and fix them if necessary */
		m_specularReflectance = ensureEnergyConservation(
			m_specularReflectance, "specularReflectance", 1.0f);
		m_specularTransmittance = ensureEnergyConservation(
			m_specularTransmittance, "specularTransmittance", 1.0f);

		m_usesRayDifferentials = 
			m_alphaU->usesRayDifferentials() ||
			m_alphaV->usesRayDifferentials() ||
			m_specularReflectance->usesRayDifferentials() ||
			m_specularTransmittance->usesRayDifferentials();

		BSDF::configure();
	}

	inline Float signum(Float value) const {
		return (value < 0) ? -1.0f : 1.0f;
	}

	/// Helper function: reflect \c wi with respect to a given surface normal
	inline Vector reflect(const Vector &wi, const Normal &m) const {
		return 2 * dot(wi, m) * Vector(m) - wi;
	}

	/// Helper function: refract \c wi with respect to a given surface normal
	inline bool refract(const Vector &wi, Vector &wo, const Normal &m, Float etaI, Float etaT) const {
		Float eta = etaI / etaT, c = dot(wi, m);

		/* Using Snell's law, calculate the squared cosine of the
		   angle between the normal and the transmitted ray */
		Float cosThetaTSqr = 1 + eta * eta * (c*c-1);

		if (cosThetaTSqr < 0) 
			return false; // Total internal reflection

		/* Compute the transmitted direction */
		wo = m * (eta*c - signum(wi.z)
			   * std::sqrt(cosThetaTSqr)) - wi * eta;

		return true;
	}

	Spectrum eval(const BSDFQueryRecord &bRec, EMeasure measure) const {
		if (measure != ESolidAngle)
			return Spectrum(0.0f);

		/* Determine the type of interaction */
		bool reflect = Frame::cosTheta(bRec.wi) 
			* Frame::cosTheta(bRec.wo) > 0;

		/* Determine the appropriate indices of refraction */
		Float etaI = m_extIOR, etaT = m_intIOR;
		if (Frame::cosTheta(bRec.wi) < 0)
			std::swap(etaI, etaT);

		Vector H;
		if (reflect) {
			/* Stop if this component was not requested */
			if ((bRec.component != -1 && bRec.component != 0)
				|| !(bRec.typeMask & EGlossyReflection))
				return Spectrum(0.0f);

			/* Calculate the reflection half-vector (and possibly flip it
			   so that it lies inside the hemisphere around the normal) */
			H = normalize(bRec.wo+bRec.wi) 
				* signum(Frame::cosTheta(bRec.wo));
		} else {
			/* Stop if this component was not requested */
			if ((bRec.component != -1 && bRec.component != 1)
				|| !(bRec.typeMask & EGlossyTransmission))
				return Spectrum(0.0f);

			/* Calculate the transmission half-vector (and possibly flip it
			   when the surface normal points into the denser medium -- this
			   removes an assumption in the original paper) */
			H = (m_extIOR > m_intIOR ? (Float) 1 : (Float) -1)
				* normalize(bRec.wi*etaI + bRec.wo*etaT);
		}

		/* Evaluate the roughness */
		Float alphaU = m_distribution.transformRoughness( 
					m_alphaU->getValue(bRec.its).average()),
			  alphaV = m_distribution.transformRoughness( 
					m_alphaV->getValue(bRec.its).average());

		/* Evaluate the microsurface normal distribution */
		const Float D = m_distribution.eval(H, alphaU, alphaV);
		if (D == 0)
			return Spectrum(0.0f);

		/* Fresnel factor */
		const Float F = fresnel(dot(bRec.wi, H), m_extIOR, m_intIOR);

		/* Smith's shadow-masking function */
		const Float G = m_distribution.G(bRec.wi, bRec.wo, H, alphaU, alphaV);

		if (reflect) {
			/* Calculate the total amount of reflection */
			Float value = F * D * G / 
				(4.0f * std::abs(Frame::cosTheta(bRec.wi)));

			return m_specularReflectance->getValue(bRec.its) * value; 
		} else {
			/* Calculate the total amount of transmission */
			Float sqrtDenom = etaI * dot(bRec.wi, H) + etaT * dot(bRec.wo, H);
			Float value = ((1 - F) * D * G * etaT * etaT 
				* dot(bRec.wi, H) * dot(bRec.wo, H)) / 
				(Frame::cosTheta(bRec.wi) * sqrtDenom * sqrtDenom);

			/* Missing term in the original paper: account for the solid angle 
			   compression when tracing radiance -- this is necessary for
			   bidirectional method */
			if (bRec.quantity == ERadiance)
				value *= (etaI*etaI) / (etaT*etaT);

			return m_specularTransmittance->getValue(bRec.its) * std::abs(value);
		}
	}

	Float pdf(const BSDFQueryRecord &bRec, EMeasure measure) const {
		if (measure != ESolidAngle)
			return 0.0f;

		/* Determine the type of interaction */
		bool hasReflection   = ((bRec.component == -1 || bRec.component == 0)
							  && (bRec.typeMask & EGlossyReflection)),
		     hasTransmission = ((bRec.component == -1 || bRec.component == 1)
							  && (bRec.typeMask & EGlossyTransmission)),
		     reflect            = Frame::cosTheta(bRec.wi) 
				                * Frame::cosTheta(bRec.wo) > 0;

		/* Determine the appropriate indices of refraction */
		Float etaI = m_extIOR, etaT = m_intIOR;
		if (Frame::cosTheta(bRec.wi) < 0)
			std::swap(etaI, etaT);

		Vector H;
		Float dwh_dwo;

		if (reflect) {
			/* Zero probability if this component was not requested */
			if ((bRec.component != -1 && bRec.component != 0)
				|| !(bRec.typeMask & EGlossyReflection))
				return 0.0f;

			/* Calculate the reflection half-vector (and possibly flip it
			   so that it lies inside the hemisphere around the normal) */
			H = normalize(bRec.wo+bRec.wi) 
				* signum(Frame::cosTheta(bRec.wo));
	
			/* Jacobian of the half-direction transform */
			dwh_dwo = 1.0f / (4.0f * dot(bRec.wo, H));
		} else {
			/* Zero probability if this component was not requested */
			if ((bRec.component != -1 && bRec.component != 1)
				|| !(bRec.typeMask & EGlossyTransmission))
				return 0.0f;

			/* Calculate the transmission half-vector (and possibly flip it
			   when the surface normal points into the denser medium -- this
			   removes an assumption in the original paper) */
			H = (m_extIOR > m_intIOR ? (Float) 1 : (Float) -1)
				* normalize(bRec.wi*etaI + bRec.wo*etaT);

			/* Jacobian of the half-direction transform. */
			Float sqrtDenom = etaI * dot(bRec.wi, H) + etaT * dot(bRec.wo, H);
			dwh_dwo = (etaT*etaT * dot(bRec.wo, H)) / (sqrtDenom*sqrtDenom);
		}

		/* Evaluate the roughness */
		Float alphaU = m_distribution.transformRoughness( 
					m_alphaU->getValue(bRec.its).average()),
			  alphaV = m_distribution.transformRoughness( 
					m_alphaV->getValue(bRec.its).average());

#if ENLARGE_LOBE_TRICK == 1
		Float factor = (1.2f - 0.2f * std::sqrt(
			std::abs(Frame::cosTheta(bRec.wi))));
		alphaU *= factor; alphaV *= factor;
#endif

		/* Evaluate the microsurface normal sampling density */
		Float prob = m_distribution.pdf(H, alphaU, alphaV);

		if (hasTransmission && hasReflection) {
			Float F = fresnel(dot(bRec.wi, H), m_extIOR, m_intIOR);
			prob *= reflect ? F : (1-F);
		}

		return std::abs(prob * dwh_dwo);
	}

	Spectrum sample(BSDFQueryRecord &bRec, const Point2 &_sample) const {
		Point2 sample(_sample);

		bool hasReflection = ((bRec.component == -1 || bRec.component == 0)
							  && (bRec.typeMask & EGlossyReflection)),
		     hasTransmission = ((bRec.component == -1 || bRec.component == 1)
							  && (bRec.typeMask & EGlossyTransmission)),
		     choseReflection = hasReflection;

		if (!hasReflection && !hasTransmission)
			return Spectrum(0.0f);

		/* Evaluate the roughness */
		Float alphaU = m_distribution.transformRoughness( 
					m_alphaU->getValue(bRec.its).average()),
			  alphaV = m_distribution.transformRoughness( 
					m_alphaV->getValue(bRec.its).average());

#if ENLARGE_LOBE_TRICK == 1
		Float factor = (1.2f - 0.2f * std::sqrt(
			std::abs(Frame::cosTheta(bRec.wi))));
		Float sampleAlphaU = alphaU * factor,
			  sampleAlphaV = alphaV * factor;
#else
		Float sampleAlphaU = alphaU,
			  sampleAlphaV = alphaV;
#endif

		/* Sample M, the microsurface normal */
		const Normal m = m_distribution.sample(sample,
				sampleAlphaU, sampleAlphaV);

		if (hasReflection && hasTransmission) {
			Float F = fresnel(dot(bRec.wi, m), m_extIOR, m_intIOR);
			if (bRec.sampler->next1D() > F)
				choseReflection = false;
		}

		Spectrum result;
		if (choseReflection) {
			/* Perfect specular reflection based on the microsurface normal */
			bRec.wo = reflect(bRec.wi, m);
			bRec.sampledComponent = 0;
			bRec.sampledType = EGlossyReflection;

			/* Side check */
			if (Frame::cosTheta(bRec.wi) * Frame::cosTheta(bRec.wo) <= 0)
				return Spectrum(0.0f);
		
			result = m_specularReflectance->getValue(bRec.its);
		} else {
			/* Determine the appropriate indices of refraction */
			Float etaI = m_extIOR, etaT = m_intIOR;
			if (Frame::cosTheta(bRec.wi) < 0)
				std::swap(etaI, etaT);

			/* Perfect specular transmission based on the microsurface normal */
			if (!refract(bRec.wi, bRec.wo, m, etaI, etaT))
				return Spectrum(0.0f);

			bRec.sampledComponent = 1;
			bRec.sampledType = EGlossyTransmission;
		
			/* Side check */
			if (Frame::cosTheta(bRec.wi) * Frame::cosTheta(bRec.wo) >= 0)
				return Spectrum(0.0f);

			result = m_specularTransmittance->getValue(bRec.its)
				* ((bRec.quantity == ERadiance) ?
					((etaI*etaI) / (etaT*etaT)) : (Float) 1);
		}

		Float numerator = m_distribution.eval(m, alphaU, alphaV)
			* m_distribution.G(bRec.wi, bRec.wo, m, alphaU, alphaV)
			* dot(bRec.wi, m);

		Float denominator = m_distribution.pdf(m, sampleAlphaU, sampleAlphaV)
			* Frame::cosTheta(bRec.wi);

		return result * std::abs(numerator / denominator);
	}

	Spectrum sample(BSDFQueryRecord &bRec, Float &_pdf, const Point2 &_sample) const {
		Point2 sample(_sample);

		bool hasReflection = ((bRec.component == -1 || bRec.component == 0)
							  && (bRec.typeMask & EGlossyReflection)),
		     hasTransmission = ((bRec.component == -1 || bRec.component == 1)
							  && (bRec.typeMask & EGlossyTransmission)),
		     choseReflection = hasReflection;

		if (!hasReflection && !hasTransmission)
			return Spectrum(0.0f);

		/* Evaluate the roughness */
		Float alphaU = m_distribution.transformRoughness( 
					m_alphaU->getValue(bRec.its).average()),
			  alphaV = m_distribution.transformRoughness( 
					m_alphaV->getValue(bRec.its).average());

#if ENLARGE_LOBE_TRICK == 1
		Float factor = (1.2f - 0.2f * std::sqrt(
			std::abs(Frame::cosTheta(bRec.wi))));
		Float sampleAlphaU = alphaU * factor,
			  sampleAlphaV = alphaV * factor;
#else
		Float sampleAlphaU = alphaU,
			  sampleAlphaV = alphaV;
#endif

		/* Sample M, the microsurface normal */
		const Normal m = m_distribution.sample(sample,
				sampleAlphaU, sampleAlphaV);

		if (hasReflection && hasTransmission) {
			Float F = fresnel(dot(bRec.wi, m), m_extIOR, m_intIOR);
			if (bRec.sampler->next1D() > F)
				choseReflection = false;
		}

		if (choseReflection) {
			/* Perfect specular reflection based on the microsurface normal */
			bRec.wo = reflect(bRec.wi, m);
			bRec.sampledComponent = 0;
			bRec.sampledType = EGlossyReflection;

			/* Side check */
			if (Frame::cosTheta(bRec.wi) * Frame::cosTheta(bRec.wo) <= 0)
				return Spectrum(0.0f);
		} else {
			/* Determine the appropriate indices of refraction */
			Float etaI = m_extIOR, etaT = m_intIOR;
			if (Frame::cosTheta(bRec.wi) < 0)
				std::swap(etaI, etaT);

			/* Perfect specular transmission based on the microsurface normal */
			if (!refract(bRec.wi, bRec.wo, m, etaI, etaT))
				return Spectrum(0.0f);

			bRec.sampledComponent = 1;
			bRec.sampledType = EGlossyTransmission;
		
			/* Side check */
			if (Frame::cosTheta(bRec.wi) * Frame::cosTheta(bRec.wo) >= 0)
				return Spectrum(0.0f);
		}

		/* Guard against numerical imprecisions */
		_pdf = pdf(bRec, ESolidAngle);

		if (_pdf == 0) 
			return Spectrum(0.0f);
		else
			return eval(bRec, ESolidAngle);
	}

	void addChild(const std::string &name, ConfigurableObject *child) {
		if (child->getClass()->derivesFrom(MTS_CLASS(Texture))) {
			if (name == "alpha")
				m_alphaU = m_alphaV = static_cast<Texture *>(child);
			else if (name == "alphaU")
				m_alphaU = static_cast<Texture *>(child);
			else if (name == "alphaV")
				m_alphaV = static_cast<Texture *>(child);
			else if (name == "specularReflectance")
				m_specularReflectance = static_cast<Texture *>(child);
			else if (name == "specularTransmittance")
				m_specularTransmittance = static_cast<Texture *>(child);
			else
				BSDF::addChild(name, child);
		} else {
			BSDF::addChild(name, child);
		}
	}

	void serialize(Stream *stream, InstanceManager *manager) const {
		BSDF::serialize(stream, manager);

		stream->writeUInt((uint32_t) m_distribution.getType());
		manager->serialize(stream, m_alphaU.get());
		manager->serialize(stream, m_alphaV.get());
		manager->serialize(stream, m_specularReflectance.get());
		manager->serialize(stream, m_specularTransmittance.get());
		stream->writeFloat(m_intIOR);
		stream->writeFloat(m_extIOR);
	}

	std::string toString() const {
		std::ostringstream oss;
		oss << "RoughDielectric[" << endl
			<< "  name = \"" << getName() << "\"," << endl
			<< "  distribution = " << m_distribution.toString() << "," << endl
			<< "  alphaU = " << indent(m_alphaU->toString()) << "," << endl
			<< "  alphaV = " << indent(m_alphaV->toString()) << "," << endl
			<< "  specularReflectance = " << indent(m_specularReflectance->toString()) << "," << endl
			<< "  specularTransmittance = " << indent(m_specularTransmittance->toString()) << "," << endl
			<< "  intIOR = " << m_intIOR << "," << endl
			<< "  extIOR = " << m_extIOR << endl
			<< "]";
		return oss.str();
	}

	Shader *createShader(Renderer *renderer) const;

	MTS_DECLARE_CLASS()
private:
	MicrofacetDistribution m_distribution;
	ref<Texture> m_specularTransmittance;
	ref<Texture> m_specularReflectance;
	ref<Texture> m_alphaU, m_alphaV;
	Float m_intIOR, m_extIOR;
};

/* Fake dielectric shader -- it is really hopeless to visualize
   this material in the VPL renderer, so let's try to do at least 
   something that suggests the presence of a translucent boundary */
class RoughDielectricShader : public Shader {
public:
	RoughDielectricShader(Renderer *renderer) :
		Shader(renderer, EBSDFShader) {
		m_flags = ETransparent;
	}

	void generateCode(std::ostringstream &oss,
			const std::string &evalName,
			const std::vector<std::string> &depNames) const {
		oss << "vec3 " << evalName << "(vec2 uv, vec3 wi, vec3 wo) {" << endl
			<< "    return vec3(0.08);" << endl
			<< "}" << endl
			<< endl
			<< "vec3 " << evalName << "_diffuse(vec2 uv, vec3 wi, vec3 wo) {" << endl
			<< "    return " << evalName << "(uv, wi, wo);" << endl
			<< "}" << endl;
	}
	MTS_DECLARE_CLASS()
};

Shader *RoughDielectric::createShader(Renderer *renderer) const { 
	return new RoughDielectricShader(renderer);
}

MTS_IMPLEMENT_CLASS(RoughDielectricShader, false, Shader)
MTS_IMPLEMENT_CLASS_S(RoughDielectric, false, BSDF)
MTS_EXPORT_PLUGIN(RoughDielectric, "Rough dielectric BSDF");
MTS_NAMESPACE_END