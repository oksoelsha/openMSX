#ifndef SCALEROUTPUT_HH
#define SCALEROUTPUT_HH

namespace openmsx {

template<typename Pixel> class ScalerOutput
{
public:
	virtual ~ScalerOutput() = default;

	virtual unsigned getWidth()  const = 0;
	virtual unsigned getHeight() const = 0;

	virtual Pixel* acquireLine(unsigned y) = 0;
	virtual void   releaseLine(unsigned y, Pixel* buf) = 0;
	virtual void   fillLine   (unsigned y, Pixel color) = 0;
	// TODO add copyLine() optimization

protected:
	ScalerOutput() = default;
};

} // namespace openmsx

#endif
